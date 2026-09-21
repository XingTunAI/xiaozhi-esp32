#include "boards/waveshare/esp32-p4-wifi6-dev-kit-b/pdm_pcm_gain.h"

constexpr bool Identity() {
    for (int i = -32768; i <= 32767; ++i)
        if (PdmPcmGain::Apply(i, 1) != i)
            return false;
    return true;
}
constexpr bool Monotonic(int gain) {
    for (int i = -32767; i <= 32767; ++i)
        if (PdmPcmGain::Apply(i, gain) < PdmPcmGain::Apply(i - 1, gain))
            return false;
    return true;
}
static_assert(Identity());
static_assert(Monotonic(2));
static_assert(Monotonic(4));
static_assert(PdmPcmGain::Apply(8191, 4) == 32764);
static_assert(PdmPcmGain::Apply(8192, 4) == 32767);
static_assert(PdmPcmGain::Apply(-8192, 4) == -32768);
static_assert(PdmPcmGain::Apply(-32768, 4) == -32768);
static_assert(PdmPcmGain::Apply(32767, 2) == 32767);
static_assert(PdmPcmGain::Apply(-1234, 0) == -1234);
