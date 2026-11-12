#include "boards/waveshare/esp32-p4-wifi6-dev-kit-b/k01_pcm.h"

// Slot order of ES7210 TDM4 is MIC1, MIC3, MIC2, MIC4.
constexpr int16_t kFrame[4] = {11, 33, 22, 44};
static_assert(k01::Select(kFrame, 1) == 11);
static_assert(k01::Select(kFrame, 2) == 22);
static_assert(k01::Select(kFrame, 3) == 33);
static_assert(k01::Select(kFrame, 4) == 44);
static_assert(k01::Select(kFrame, k01::kModeMean) == 27);  // (11+33+22+44)/4

// Every physical microphone maps to a distinct slot.
constexpr bool MappingIsPermutation() {
    bool seen[4] = {};
    for (int mic = 1; mic <= 4; ++mic) {
        const int slot = k01::kMicToSlot[mic];
        if (slot < 0 || slot >= k01::kSlots || seen[slot])
            return false;
        seen[slot] = true;
    }
    return true;
}
static_assert(MappingIsPermutation());

// The mean never overflows, including full-scale extremes.
constexpr int16_t kMax[4] = {32767, 32767, 32767, 32767};
constexpr int16_t kMin[4] = {-32768, -32768, -32768, -32768};
constexpr int16_t kMixed[4] = {32767, -32768, 32767, -32768};
static_assert(k01::MixMean(kMax) == 32767);
static_assert(k01::MixMean(kMin) == -32768);
static_assert(k01::MixMean(kMixed) == 0);

static_assert(k01::ValidMode(0) && k01::ValidMode(4) && !k01::ValidMode(-1) && !k01::ValidMode(5));
static_assert(k01::ValidGain(0) && k01::ValidGain(375) && !k01::ValidGain(376) && !k01::ValidGain(-1));
static_assert(k01::ValidGain(k01::kDefaultGainTenths));
static_assert(k01::kFrame * 1000 / k01::kRate == 20);

// Four-channel evidence: one TDM4 frame becomes physical MIC1..MIC4 ("k01_mic_1_4").
constexpr bool DeinterleaveIsPhysicalOrder() {
    int16_t out[4] = {};
    k01::Deinterleave(kFrame, out);
    return out[0] == 11 && out[1] == 22 && out[2] == 33 && out[3] == 44;
}
static_assert(DeinterleaveIsPhysicalOrder());
