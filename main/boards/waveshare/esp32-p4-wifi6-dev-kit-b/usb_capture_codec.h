#pragma once

#include <memory>
#include "audio/audio_codec.h"
#include "board_peripherals.h"
#include "pdm_capture.h"
#include "settings.h"

// Preference is latched at the next recording boundary, never mid-stream.
inline std::atomic<bool> board_pdm_selected{false};
inline std::atomic<int> board_pdm_mode{0};
inline std::atomic<int> board_pdm_gain{1};
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
inline bool BoardPdmSelected() { return board_pdm_selected.load(); }
inline void SelectBoardPdm(bool pdm) {
    Settings settings("p4_audio", true);
    settings.SetBool("pdm", pdm);
    board_pdm_selected.store(pdm);
}

// Keep onboard prompt playback with session-scoped PDM/USB capture selection.
class UsbCaptureCodec : public AudioCodec {
public:
    explicit UsbCaptureCodec(AudioCodec* playback) : playback_(playback) {
        input_sample_rate_ = 16000;
        output_sample_rate_ = playback_->output_sample_rate();
        duplex_ = true;
        Settings settings("p4_audio");
        board_pdm_selected.store(settings.GetBool("pdm", false));
        const int mode = settings.GetInt("pdm_mode", 0);
        board_pdm_mode.store(mode >= 0 && mode <= 2 ? mode : 0);
        const int gain = settings.GetInt("pdm_gain", 1);
        board_pdm_gain.store(PdmPcmGain::Valid(gain) ? gain : 1);
    }
    void Start() override {
        playback_->Start();
        output_volume_ = playback_->output_volume();
    }
    bool InputData(std::vector<int16_t>& data) override {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (!input_enabled_)
            return false;
        if (!use_pdm_)
            return ReadBoardUsbAudio(data.data(), data.size());
        if (!pdm_.Start(pdm_mode_, pdm_gain_)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            return false;
        }
        return pdm_.Fetch(data.data(), data.size());
    }
    void ResetInputCapture() override {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        StopBoardUsbAudio();
        pdm_.Stop();
        use_pdm_ = BoardPdmSelected();
        pdm_mode_ = board_pdm_mode.load();
        pdm_gain_ = board_pdm_gain.load();
    }
    void EnableInput(bool enable) override {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        if (!enable) {
            StopBoardUsbAudio();
            pdm_.Stop();
        }
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
    bool use_pdm_ = false;
    int pdm_mode_ = 0;
    int pdm_gain_ = 1;
    std::unique_ptr<AudioCodec> playback_;
};
