#pragma once

#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>
#include <esp_codec_dev_defaults.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include "audio/audio_codec.h"  // XIAOZHI_I2S_PORT: IDF 6 removed i2s_port_t
#include "k01_pcm.h"

// XingTun K01 analog four-mic front end (ES7210 TDM4) on the 40-pin header.
// Wiring and first bring-up: voice-lab/docs/tech/ESP32-P4-WIFI6-DEV-KIT/K01-AUDIO-BOARD-WIRING.md
// Bring-up probe: voice-lab/firmware/p4-k01-probe. Pins, I2S format and codec settings are
// copied from the probe that passed on 2026-11-04 (I2C IDs, 8 s TDM capture, 0 errors).
//
// Same contract as PdmCapture: owned by the board for its lifetime, Start() is latched per
// recording, Fetch() returns exactly one 20 ms frame or false: mono (the selected mic or the
// mean), or all four mics interleaved in physical MIC1..MIC4 order for Voice Lab evidence.
// Raw only: no AFE, no digital gain, no automatic fallback to another source.
namespace k01 {
constexpr i2c_port_t kI2cPort = I2C_NUM_0;  // board I2C1 (GPIO7/8) also has an ES8311 at 0x18
constexpr gpio_num_t kSda = GPIO_NUM_26;
constexpr gpio_num_t kScl = GPIO_NUM_27;
constexpr int kI2sPort = 2;  // I2S0 = PDM source, I2S1 = on-board ES8311 playback
constexpr gpio_num_t kMclk = GPIO_NUM_20;
constexpr gpio_num_t kBclk = GPIO_NUM_21;
constexpr gpio_num_t kWs = GPIO_NUM_23;
constexpr gpio_num_t kDin = GPIO_NUM_22;
}  // namespace k01

class K01Capture {
public:
    bool Start(int mode, int gain_tenths) {
        if (!k01::ValidMode(mode) || !k01::ValidGain(gain_tenths))
            return false;
        // Called for every 20 ms request; never block on the I2S read lock once running.
        if (running_.load(std::memory_order_acquire))
            return true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_)
            return true;
        if (!rx_ && !InitializeRx())
            return false;
        if (i2s_channel_enable(rx_) != ESP_OK)
            return false;
        // MCLK/BCLK/LRCK run from here, so the codec is (re)configured with clocks present,
        // matching the order that passed in the probe.
        if (!adc_ && !InitializeCodec()) {
            i2s_channel_disable(rx_);
            return false;
        }
        const float gain_db = gain_tenths / 10.0f;
        // Order matters: esp_codec_dev's ES7210 enable() -> es7210_start() -> mic_select()
        // rewrites REG43-46 from an internal gain field that set_mic_gain() never updates
        // (it stays GAIN_0DB). Setting the gain first was silently undone, so every capture
        // before 2026-11-11 23:10 ran at 0 dB PGA. Enable first, then set the gain.
        if (adc_->enable(adc_, true) != ESP_CODEC_DEV_OK ||
            adc_->set_mic_gain(adc_, gain_db) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(kTag, "ES7210 enable/gain failed");
            i2s_channel_disable(rx_);
            return false;
        }
        mode_ = mode;
        gain_tenths_ = gain_tenths;
        raw_bytes_ = 0;
        ResetLevels();
        // Same settling as the P0/PDM path: discard 10 x 20 ms before the first delivered
        // sample, bounded so an absent board cannot trap Start().
        const int64_t deadline = esp_timer_get_time() + 500000;
        size_t warm_bytes = 0;
        constexpr size_t warm_target = 10 * sizeof(raw_);
        while (warm_bytes < warm_target && esp_timer_get_time() < deadline) {
            size_t got = 0;
            auto err = i2s_channel_read(rx_, raw_.data(),
                                        std::min(sizeof(raw_), warm_target - warm_bytes), &got, 100);
            if ((err != ESP_OK && err != ESP_ERR_TIMEOUT) || got % (k01::kSlots * 2) != 0)
                break;
            warm_bytes += got;
        }
        if (warm_bytes != warm_target) {
            adc_->enable(adc_, false);
            i2s_channel_disable(rx_);
            ESP_LOGE(kTag, "K01 warmup incomplete: %u/%u bytes", static_cast<unsigned>(warm_bytes),
                     static_cast<unsigned>(warm_target));
            return false;
        }
        overflows_ = 0;
        running_ = true;
        // Read back REG43-46 so the log proves the PGA actually in effect (low nibble:
        // 0=0 dB ... 10=30 dB, 11=33, 12=34.5, 13=36, 14=37.5).
        uint8_t pga[4] = {};
        for (int i = 0; i < 4; ++i)
            ctrl_->read_reg(ctrl_, 0x43 + i, 1, &pga[i], 1);
        ESP_LOGI(kTag,
                 "K01 capture started: %s, PGA %.1f dB (REG43-46=%02x/%02x/%02x/%02x), "
                 "TDM4 16 kHz, warmup 200 ms",
                 ModeName(mode_), gain_db, pga[0], pga[1], pga[2], pga[3]);
        return true;
    }

    void Stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            running_ = false;
            adc_->enable(adc_, false);
            i2s_channel_disable(rx_);
        }
        raw_bytes_ = 0;
    }

    // `count` is samples per channel; `channels` is 1 or 4.
    bool Fetch(int16_t* output, size_t count, int channels = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || count != k01::kFrame || (channels != 1 && channels != k01::kSlots))
            return false;
        size_t bytes = 0;
        auto err = i2s_channel_read(rx_, reinterpret_cast<uint8_t*>(raw_.data()) + raw_bytes_,
                                    sizeof(raw_) - raw_bytes_, &bytes, 100);
        raw_bytes_ += bytes;
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            raw_bytes_ = 0;
            ++read_errors_;
            return false;
        }
        if (raw_bytes_ != sizeof(raw_))
            return false;  // partial frame kept for the next call
        raw_bytes_ = 0;
        for (size_t i = 0; i < count; ++i) {
            const int16_t* frame = &raw_[i * k01::kSlots];
            for (int s = 0; s < k01::kSlots; ++s) {
                const int32_t v = frame[s];
                energy_[s] += static_cast<uint64_t>(v * v);
                peak_[s] = std::max(peak_[s], v < 0 ? -v : v);
            }
            if (channels == 1)
                output[i] = k01::Select(frame, mode_);
            else
                k01::Deinterleave(frame, &output[i * k01::kSlots]);
        }
        level_samples_ += count;
        if (level_samples_ >= k01::kRate) {
            // Per-microphone levels let a tap test confirm each mic and the slot mapping.
            ESP_LOGI(kTag,
                     "K01 %s ms[MIC1..4]=%llu/%llu/%llu/%llu peak=%ld/%ld/%ld/%ld "
                     "read_errors=%lu overflow=%lu",
                     ModeName(mode_), Ms(1), Ms(2), Ms(3), Ms(4), Peak(1), Peak(2), Peak(3),
                     Peak(4), static_cast<unsigned long>(read_errors_),
                     static_cast<unsigned long>(overflows_.load()));
            ResetLevels();
        }
        return true;
    }

    static const char* ModeName(int mode) {
        static const char* names[] = {"K01 mean4", "K01 MIC1", "K01 MIC2", "K01 MIC3", "K01 MIC4"};
        return k01::ValidMode(mode) ? names[mode] : "K01 invalid";
    }

private:
    static constexpr char kTag[] = "P4K01";
    int mode_ = 1;
    int gain_tenths_ = k01::kDefaultGainTenths;
    std::array<int16_t, k01::kFrame * k01::kSlots> raw_{};
    size_t raw_bytes_ = 0;
    i2c_master_bus_handle_t bus_ = nullptr;
    i2s_chan_handle_t rx_ = nullptr;
    const audio_codec_if_t* adc_ = nullptr;
    const audio_codec_ctrl_if_t* ctrl_ = nullptr;
    std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> overflows_{0};
    uint32_t read_errors_ = 0;
    uint32_t level_samples_ = 0;
    std::array<uint64_t, k01::kSlots> energy_{};
    std::array<int32_t, k01::kSlots> peak_{};

    unsigned long long Ms(int mic) const {
        return level_samples_ ? energy_[k01::kMicToSlot[mic]] / level_samples_ : 0;
    }
    long Peak(int mic) const { return peak_[k01::kMicToSlot[mic]]; }
    void ResetLevels() {
        level_samples_ = 0;
        energy_.fill(0);
        peak_.fill(0);
    }

    bool InitializeRx() {
        i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(k01::kI2sPort), I2S_ROLE_MASTER);
        channel.dma_desc_num = 8;
        channel.dma_frame_num = k01::kFrame;
        esp_err_t err = i2s_new_channel(&channel, nullptr, &rx_);
        if (err == ESP_OK) {
            i2s_event_callbacks_t callbacks{};
            callbacks.on_recv_q_ovf = [](i2s_chan_handle_t, i2s_event_data_t*, void* arg) {
                static_cast<K01Capture*>(arg)->overflows_.fetch_add(1, std::memory_order_relaxed);
                return false;
            };
            err = i2s_channel_register_event_callback(rx_, &callbacks, this);
        }
        if (err == ESP_OK) {
            i2s_tdm_config_t tdm{};
            tdm.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(k01::kRate);
            tdm.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
            tdm.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3));
            tdm.gpio_cfg.mclk = k01::kMclk;
            tdm.gpio_cfg.bclk = k01::kBclk;
            tdm.gpio_cfg.ws = k01::kWs;
            tdm.gpio_cfg.dout = I2S_GPIO_UNUSED;
            tdm.gpio_cfg.din = k01::kDin;
            err = i2s_channel_init_tdm_mode(rx_, &tdm);
        }
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "K01 I2S init failed: %s", esp_err_to_name(err));
            if (rx_) {
                i2s_del_channel(rx_);
                rx_ = nullptr;
            }
            return false;
        }
        // Undriven SDOUT reads as zero rather than noise, so a silent ES7210 is obvious.
        gpio_pulldown_en(k01::kDin);
        return true;
    }

    bool InitializeCodec() {
        if (!bus_) {
            i2c_master_bus_config_t config = {};
            config.i2c_port = k01::kI2cPort;
            config.sda_io_num = k01::kSda;
            config.scl_io_num = k01::kScl;
            config.clk_source = I2C_CLK_SRC_DEFAULT;
            config.glitch_ignore_cnt = 7;
            config.flags.enable_internal_pullup = false;  // K01 has 4.7k pull-ups to its own 3.3V
            if (i2c_new_master_bus(&config, &bus_) != ESP_OK) {
                ESP_LOGE(kTag, "K01 I2C bus init failed");
                bus_ = nullptr;
                return false;
            }
        }
        if (i2c_master_probe(bus_, 0x40, 50) != ESP_OK) {
            ESP_LOGE(kTag, "ES7210 not found at 0x40: check K01 power and CN1-4/5");
            return false;
        }
        audio_codec_i2c_cfg_t i2c_cfg = {};
        i2c_cfg.port = k01::kI2cPort;
        i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
        i2c_cfg.bus_handle = bus_;
        const audio_codec_ctrl_if_t* ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
        if (!ctrl)
            return false;
        es7210_codec_cfg_t cfg = {};
        cfg.ctrl_if = ctrl;
        cfg.master_mode = false;
        cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
        cfg.mclk_src = ES7210_MCLK_FROM_PAD;
        cfg.mclk_div = 256;
        const audio_codec_if_t* adc = es7210_codec_new(&cfg);
        esp_codec_dev_sample_info_t fs = {};
        fs.bits_per_sample = 16;
        fs.channel = k01::kSlots;
        fs.channel_mask = 0x0F;
        fs.sample_rate = k01::kRate;
        fs.mclk_multiple = 256;
        if (!adc || adc->set_fs(adc, &fs) != ESP_CODEC_DEV_OK) {
            // Release everything so the next recording retries from a clean state.
            ESP_LOGE(kTag, "ES7210 %s failed", adc ? "set_fs" : "open");
            if (adc)
                audio_codec_delete_codec_if(adc);
            audio_codec_delete_ctrl_if(ctrl);
            return false;
        }
        adc_ = adc;
        ctrl_ = ctrl;
        ESP_LOGI(kTag, "K01 ready: ES7210 TDM4, I2C0 GPIO26/27, I2S2 MCLK20 BCLK21 WS23 DIN22");
        return true;
    }
};
