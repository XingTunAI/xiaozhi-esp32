#pragma once

#include <driver/i2s_pdm.h>
#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
#include "pdm_pcm_gain.h"

// Ported from voice-lab/firmware/p4-voice-terminal/main/audio.c.
// The board owns this object for its entire lifetime. Only the audio input
// task consumes Fetch(); the feeder is separate from network and storage.
class PdmCapture {
public:
    // 0/1: unprocessed PDM slots, 2: MM enhancement. Latched per recording.
    bool Start(int mode = 0, int gain = 1) {
        if (mode < 0 || mode > 2 || !PdmPcmGain::Valid(gain))
            return false;
        // ReadAudioData calls this every 20 ms. Do not wait for the feeder's
        // blocking 64 ms I2S read once capture is running: that starves fetch.
        if (running_.load(std::memory_order_acquire))
            return true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_)
            return true;
        if (!rx_ && !InitializeRx())
            return false;
        if (mode == 2 && !afe_data_ && !InitializeAfe())
            return false;
        mode_ = mode;
        gain_ = mode == 2 ? 1 : gain;
        level_samples_ = clipped_ = 0;
        input_energy_ = 0;
        input_peak_ = 0;
        raw_bytes_ = 0;
        if (afe_data_)
            afe_->reset_buffer(afe_data_);
        pending_.clear();
        offset_ = 0;
        if (i2s_channel_enable(rx_) != ESP_OK)
            return false;
        // Restore the P0 probe's 10 x 20 ms settling reads. Only subsequent
        // samples reach the recording callback and receive sample indices.
        // Bound failures so an absent/stalled microphone cannot trap Start().
        const int64_t deadline = esp_timer_get_time() + 500000;
        size_t warm_bytes = 0;
        constexpr size_t warm_target = 10 * sizeof(raw_);
        while (warm_bytes < warm_target && esp_timer_get_time() < deadline) {
            size_t got = 0;
            auto err = i2s_channel_read(
                rx_, raw_.data(), std::min(sizeof(raw_), warm_target - warm_bytes), &got, 100);
            if ((err != ESP_OK && err != ESP_ERR_TIMEOUT) || got % 4 != 0)
                break;
            warm_bytes += got;
        }
        if (warm_bytes != warm_target) {
            i2s_channel_disable(rx_);
            ESP_LOGE("P4Pdm", "PDM warmup incomplete: %u/%u bytes",
                     static_cast<unsigned>(warm_bytes), static_cast<unsigned>(warm_target));
            return false;
        }
        overflows_ = 0;
        ESP_LOGI("P4Pdm", "P0 warmup complete: 3200 stereo frames (200ms); DMA=8x320");
        running_ = true;
        ESP_LOGI("P4Pdm", "Capture mode: %s",
                 mode_ == 2   ? "MM AFE"
                 : mode_ == 0 ? "raw slot 0"
                              : "raw slot 1");
        ESP_LOGI("P4Pdm", "Capture gain: x%d (latched)", gain_);
        if (mode_ == 2)
            xTaskNotifyGive(task_);
        return true;
    }

    void Stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            running_ = false;
            i2s_channel_disable(rx_);
        }
        pending_.clear();
        offset_ = 0;
    }

    bool Fetch(int16_t* output, size_t count) {
        if (mode_ != 2) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ || count != raw_.size() / 2)
                return false;
            size_t bytes = 0;
            auto err = i2s_channel_read(rx_, reinterpret_cast<uint8_t*>(raw_.data()) + raw_bytes_,
                                        sizeof(raw_) - raw_bytes_, &bytes, 100);
            raw_bytes_ += bytes;
            if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
                raw_bytes_ = 0;
                return false;
            }
            if (raw_bytes_ != sizeof(raw_))
                return false;
            for (size_t i = 0; i < count; ++i) {
                const int32_t sample = raw_[2 * i + mode_];
                input_energy_ += static_cast<uint64_t>(sample * sample);
                input_peak_ = std::max(input_peak_, sample < 0 ? -sample : sample);
                const int32_t scaled = sample * gain_;
                clipped_ += scaled > 32767 || scaled < -32768;
                output[i] = PdmPcmGain::Apply(sample, gain_);
            }
            level_samples_ += count;
            if (level_samples_ >= 16000) {
                ESP_LOGI(
                    "P4Pdm",
                    "slot=%d gain=%d samples=%lu input_ms=%llu peak=%ld clipped=%lu overflow=%lu",
                    mode_, gain_, static_cast<unsigned long>(level_samples_),
                    static_cast<unsigned long long>(input_energy_ / level_samples_),
                    static_cast<long>(input_peak_), static_cast<unsigned long>(clipped_),
                    static_cast<unsigned long>(overflows_.load()));
                level_samples_ = clipped_ = 0;
                input_energy_ = 0;
                input_peak_ = 0;
            }
            raw_bytes_ = 0;
            return true;
        }
        size_t used = 0;
        while (used < count) {
            if (offset_ == pending_.size()) {
                auto* result = afe_->fetch_with_delay(afe_data_, pdMS_TO_TICKS(100));
                if (!result || result->ret_value != ESP_OK || result->data_size <= 0)
                    return false;
                pending_.assign(result->data, result->data + result->data_size / sizeof(int16_t));
                offset_ = 0;
            }
            const size_t n = std::min(count - used, pending_.size() - offset_);
            std::copy_n(pending_.data() + offset_, n, output + used);
            offset_ += n;
            used += n;
        }
        return true;
    }

private:
    int mode_ = 0;
    int gain_ = 1;
    uint32_t level_samples_ = 0, clipped_ = 0;
    uint64_t input_energy_ = 0;
    int32_t input_peak_ = 0;
    std::array<int16_t, 640> raw_{};
    size_t raw_bytes_ = 0;
    i2s_chan_handle_t rx_ = nullptr;
    const esp_afe_sr_iface_t* afe_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::vector<int16_t> feed_, pending_;
    size_t offset_ = 0;
    std::atomic<uint32_t> overflows_{0};
    uint32_t read_errors_ = 0;

    bool InitializeAfe() {
        // Optional experimental MM path. Raw P0 capture never creates AFE.
        auto* cfg = afe_config_init("MM", nullptr, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
        if (!cfg)
            return false;
        cfg->aec_init = false;
        cfg->se_init = true;
        cfg->ns_init = true;
        cfg->vad_init = false;
        cfg->wakenet_init = false;
        cfg->agc_init = false;
        cfg->afe_perferred_core = 1;
        cfg->afe_perferred_priority = 6;
        cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
        cfg->afe_ringbuf_size = 50;
        afe_ = esp_afe_handle_from_config(cfg);
        afe_data_ = afe_ ? afe_->create_from_config(cfg) : nullptr;
        afe_config_free(cfg);
        if (!afe_data_)
            return false;
        feed_.resize(afe_->get_feed_chunksize(afe_data_) * 2);
        if (xTaskCreatePinnedToCore([](void* arg) { static_cast<PdmCapture*>(arg)->FeedLoop(); },
                                    "p4_pdm_feed", 4096, this, 8, &task_, 0) != pdPASS) {
            afe_->destroy(afe_data_);
            afe_data_ = nullptr;
            return false;
        }
        return true;
    }

    bool InitializeRx() {
        i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        channel.dma_desc_num = 8;
        channel.dma_frame_num = 320;
        esp_err_t err = i2s_new_channel(&channel, nullptr, &rx_);
        if (err == ESP_OK) {
            i2s_event_callbacks_t callbacks{};
            callbacks.on_recv_q_ovf = [](i2s_chan_handle_t, i2s_event_data_t*, void* arg) {
                static_cast<PdmCapture*>(arg)->overflows_.fetch_add(1, std::memory_order_relaxed);
                return false;
            };
            err = i2s_channel_register_event_callback(rx_, &callbacks, this);
        }
        if (err == ESP_OK) {
            i2s_pdm_rx_config_t pdm{};
            pdm.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000);
            pdm.slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                                  I2S_SLOT_MODE_STEREO);
            pdm.gpio_cfg.clk = GPIO_NUM_2;
            pdm.gpio_cfg.dins[0] = GPIO_NUM_3;
            for (size_t i = 1; i < sizeof(pdm.gpio_cfg.dins) / sizeof(pdm.gpio_cfg.dins[0]); ++i)
                pdm.gpio_cfg.dins[i] = I2S_GPIO_UNUSED;
            err = i2s_channel_init_pdm_rx_mode(rx_, &pdm);
        }
        if (err != ESP_OK) {
            ESP_LOGE("P4Pdm", "Initialization failed: %s", esp_err_to_name(err));
            if (rx_) {
                i2s_del_channel(rx_);
                rx_ = nullptr;
            }
            return false;
        }
        ESP_LOGI("P4Pdm", "GPIO2/3 stereo 16kHz ready; raw slots or MM AFE selectable");
        return true;
    }

    void FeedLoop() {
        int64_t last_log = 0;
        uint64_t energy[2]{};
        uint32_t samples = 0;
        for (;;) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            for (;;) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!running_ || mode_ != 2)
                        break;
                    if (running_) {
                        size_t bytes = 0;
                        auto err = i2s_channel_read(rx_, feed_.data(),
                                                    feed_.size() * sizeof(int16_t), &bytes, 100);
                        if (err == ESP_OK && bytes == feed_.size() * sizeof(int16_t)) {
                            for (size_t i = 0; i < feed_.size(); ++i) {
                                const int32_t sample = feed_[i];
                                energy[i % 2] += static_cast<uint64_t>(sample * sample);
                            }
                            samples += feed_.size() / 2;
                            afe_->feed(afe_data_, feed_.data());
                        } else {
                            ++read_errors_;
                        }
                        if (esp_timer_get_time() - last_log >= 1000000) {
                            ESP_LOGI(
                                "P4Pdm",
                                "raw mean-square L=%llu R=%llu samples=%lu errors=%lu overflow=%lu",
                                static_cast<unsigned long long>(samples ? energy[0] / samples : 0),
                                static_cast<unsigned long long>(samples ? energy[1] / samples : 0),
                                static_cast<unsigned long>(samples),
                                static_cast<unsigned long>(read_errors_),
                                static_cast<unsigned long>(overflows_.load()));
                            energy[0] = energy[1] = 0;
                            samples = 0;
                            last_log = esp_timer_get_time();
                        }
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
    }
};
