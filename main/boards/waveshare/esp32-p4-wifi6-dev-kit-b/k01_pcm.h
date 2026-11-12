#pragma once

#include <cstddef>
#include <cstdint>

// Pure K01 helpers, free of ESP-IDF types so host tests can check them.
namespace k01 {
constexpr uint32_t kRate = 16000;
constexpr int kSlots = 4;
constexpr size_t kFrame = 320;  // 20 ms

// Recording channel: 1..4 = one physical microphone, 0 = mean of all four.
constexpr int kModeMean = 0;
constexpr bool ValidMode(int mode) { return mode >= 0 && mode <= 4; }
// ES7210 TDM emits MIC1, MIC3, MIC2, MIC4 (see main/audio/codecs/box_audio_codec.cc).
// Verified on C-401 on 2026-11-12: blowing into each port at PGA 0 dB raised only that mic, 19-33 dB.
constexpr int kMicToSlot[5] = {-1, 0, 2, 1, 3};
// ES7210 PGA range; stored in NVS as tenths of a dB.
constexpr int kMinGainTenths = 0;
constexpr int kMaxGainTenths = 375;
constexpr int kDefaultGainTenths = 300;
constexpr bool ValidGain(int tenths) { return tenths >= kMinGainTenths && tenths <= kMaxGainTenths; }

// Mean of the four slots: the int32 sum of four int16 values always fits, and the
// quotient always fits back into int16.
constexpr int16_t MixMean(const int16_t* frame) {
    return static_cast<int16_t>((static_cast<int32_t>(frame[0]) + frame[1] + frame[2] + frame[3]) / 4);
}

// One mono sample from an interleaved TDM4 frame for the given mode.
constexpr int16_t Select(const int16_t* frame, int mode) {
    return mode == kModeMean ? MixMean(frame) : frame[kMicToSlot[mode]];
}
// One TDM4 frame reordered to physical MIC1..MIC4 (Voice Lab layout "k01_mic_1_4").
constexpr void Deinterleave(const int16_t* frame, int16_t* out) {
    for (int mic = 1; mic <= kSlots; ++mic)
        out[mic - 1] = frame[kMicToSlot[mic]];
}
}  // namespace k01
