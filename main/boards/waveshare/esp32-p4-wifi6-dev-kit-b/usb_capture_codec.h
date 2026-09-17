#pragma once

#include <memory>
#include "audio/audio_codec.h"
#include "board_peripherals.h"

// Keep onboard prompt playback while the external UAC2 array supplies capture.
class UsbCaptureCodec : public AudioCodec {
public:
    explicit UsbCaptureCodec(AudioCodec* playback) : playback_(playback) {
        input_sample_rate_ = 16000;
        output_sample_rate_ = playback_->output_sample_rate();
        duplex_ = true;
    }
    void Start() override {
        playback_->Start();
        output_volume_ = playback_->output_volume();
    }
    bool InputData(std::vector<int16_t>& data) override {
        return input_enabled_ && ReadBoardUsbAudio(data.data(), data.size());
    }
    void ResetInputCapture() override { StopBoardUsbAudio(); }
    void EnableInput(bool enable) override {
        if (!enable)
            StopBoardUsbAudio();
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
    std::unique_ptr<AudioCodec> playback_;
};
