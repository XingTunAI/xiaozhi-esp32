#pragma once

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>
#include "audio/audio_codec.h"
#include "board_peripherals.h"
#include "k01_capture.h"
#include "pdm_capture.h"
#include "settings.h"

// Preference is latched at the next recording boundary, never mid-stream.
// NVS p4_audio/source: 0 USB, 1 on-board PDM, 2 K01. The legacy bool p4_audio/pdm is
// still written so a rollback to older firmware resolves K01 to USB rather than PDM.
constexpr int kBoardSourceUsb = 0;
constexpr int kBoardSourcePdm = 1;
constexpr int kBoardSourceK01 = 2;
inline std::atomic<int> board_audio_source{kBoardSourceUsb};
inline std::atomic<int> board_pdm_mode{0};
inline std::atomic<int> board_pdm_gain{1};
inline std::atomic<int> board_k01_mode{1};
inline std::atomic<int> board_k01_gain{k01::kDefaultGainTenths};
inline void SelectBoardPdmGain(int gain) {
    if (!PdmPcmGain::Valid(gain))
        return;
    Settings settings("p4_audio", true);
    settings.SetInt("pdm_gain", gain);
    board_pdm_gain.store(gain);
}
inline void SelectBoardPdmMode(int mode) {
    if (mode < 0 || mode > 2)
        return;
    Settings settings("p4_audio", true);
    settings.SetInt("pdm_mode", mode);
    board_pdm_mode.store(mode);
}
inline const char* BoardPdmModeName() {
    const int mode = board_pdm_mode.load();
    return mode == 0 ? "PDM L" : mode == 1 ? "PDM R" : "PDM AFE";
}
inline bool BoardPdmSelected() { return board_audio_source.load() == kBoardSourcePdm; }
inline void SelectBoardAudioSource(int source) {
    if (source < kBoardSourceUsb || source > kBoardSourceK01)
        return;
    {
        Settings settings("p4_audio", true);  // committed when this scope closes
        settings.SetInt("source", source);
        settings.SetBool("pdm", source == kBoardSourcePdm);
    }
    board_audio_source.store(source);
    // K01 I2C (GPIO26/27) and the FS H2 USB layout are mutually exclusive and fixed at
    // boot; reboot so the next recording runs on the layout the new source needs.
    if ((source == kBoardSourceK01) != BoardUsbLayoutForK01()) {
        ESP_LOGW("P4Audio", "USB layout must change for %s; restarting in 1 s",
                 source == kBoardSourceK01 ? "K01" : "USB/PDM");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}
inline void SelectBoardPdm(bool pdm) { SelectBoardAudioSource(pdm ? kBoardSourcePdm : kBoardSourceUsb); }
inline void SelectBoardK01Mode(int mode) {
    if (!k01::ValidMode(mode))
        return;
    Settings settings("p4_audio", true);
    settings.SetInt("k01_mode", mode);
    board_k01_mode.store(mode);
}
inline void SelectBoardK01Gain(int tenths) {
    if (!k01::ValidGain(tenths))
        return;
    Settings settings("p4_audio", true);
    settings.SetInt("k01_gain", tenths);
    board_k01_gain.store(tenths);
}
// ASCII only for K01: the settings page font is a fixed subset.
inline const char* BoardAudioSourceName() {
    switch (board_audio_source.load()) {
        case kBoardSourcePdm:
            return BoardPdmModeName();
        case kBoardSourceK01:
            return K01Capture::ModeName(board_k01_mode.load());
        default:
            return "USB";
    }
}

// Keep onboard prompt playback with session-scoped PDM/USB capture selection.
class UsbCaptureCodec : public AudioCodec {
public:
    explicit UsbCaptureCodec(AudioCodec* playback) : playback_(playback) {
        input_sample_rate_ = 16000;
        output_sample_rate_ = playback_->output_sample_rate();
        duplex_ = true;
        Settings settings("p4_audio");
        const int legacy = settings.GetBool("pdm", false) ? kBoardSourcePdm : kBoardSourceUsb;
        const int source = settings.GetInt("source", legacy);
        board_audio_source.store(source >= kBoardSourceUsb && source <= kBoardSourceK01 ? source
                                                                                          : legacy);
        const int mode = settings.GetInt("pdm_mode", 0);
        board_pdm_mode.store(mode >= 0 && mode <= 2 ? mode : 0);
        const int gain = settings.GetInt("pdm_gain", 1);
        board_pdm_gain.store(PdmPcmGain::Valid(gain) ? gain : 1);
        const int k01_mode = settings.GetInt("k01_mode", 1);
        board_k01_mode.store(k01::ValidMode(k01_mode) ? k01_mode : 1);
        const int k01_gain = settings.GetInt("k01_gain", k01::kDefaultGainTenths);
        board_k01_gain.store(k01::ValidGain(k01_gain) ? k01_gain : k01::kDefaultGainTenths);
    }
    void Start() override {
        playback_->Start();
        output_volume_ = playback_->output_volume();
    }
    bool InputData(std::vector<int16_t>& data) override {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (!input_enabled_)
            return false;
        // A failed source is reported to the recording path; never silently switch sources.
        switch (source_) {
            case kBoardSourcePdm:
                if (!pdm_.Start(pdm_mode_, pdm_gain_)) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    return false;
                }
                return pdm_.Fetch(data.data(), data.size());
            case kBoardSourceK01:
                if (!k01_.Start(k01_mode_, k01_gain_)) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    return false;
                }
                return k01_.Fetch(data.data(), data.size());
            default:
                return ReadBoardUsbAudio(data.data(), data.size());
        }
    }
    void ResetInputCapture() override {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        StopAllSources();
        source_ = board_audio_source.load();
        pdm_mode_ = board_pdm_mode.load();
        pdm_gain_ = board_pdm_gain.load();
        k01_mode_ = board_k01_mode.load();
        k01_gain_ = board_k01_gain.load();
    }
    void EnableInput(bool enable) override {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (!enable)
            StopAllSources();
        AudioCodec::EnableInput(enable);
    }
    void EnableOutput(bool enable) override {
        playback_->EnableOutput(enable);
        AudioCodec::EnableOutput(enable);
    }
    void OutputData(std::vector<int16_t>& data) override { playback_->OutputData(data); }
    void SetOutputVolume(int volume) override {
        playback_->SetOutputVolume(volume);
        output_volume_ = playback_->output_volume();
    }

protected:
    int Read(int16_t* samples, int count) override {
        return ReadBoardUsbAudio(samples, count) ? count : 0;
    }
    int Write(const int16_t*, int) override { return 0; }

private:
    std::mutex capture_mutex_;
    PdmCapture pdm_;
    K01Capture k01_;
    int source_ = kBoardSourceUsb;
    int pdm_mode_ = 0;
    int pdm_gain_ = 1;
    int k01_mode_ = 1;
    int k01_gain_ = k01::kDefaultGainTenths;

    void StopAllSources() {
        StopBoardUsbAudio();
        pdm_.Stop();
        k01_.Stop();
    }
    std::unique_ptr<AudioCodec> playback_;
};
