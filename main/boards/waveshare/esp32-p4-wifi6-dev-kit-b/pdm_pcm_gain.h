#pragma once

#include <cstdint>

namespace PdmPcmGain {
constexpr bool Valid(int gain) { return gain == 1 || gain == 2 || gain == 4; }

// Saturate before narrowing: an overloaded input must never wrap its sign.
constexpr int16_t Apply(int16_t sample, int gain) {
    const int32_t scaled = static_cast<int32_t>(sample) * (Valid(gain) ? gain : 1);
    return static_cast<int16_t>(scaled > 32767 ? 32767 : scaled < -32768 ? -32768 : scaled);
}
}  // namespace PdmPcmGain
