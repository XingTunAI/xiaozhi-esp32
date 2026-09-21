#pragma once

// Deliberately unassigned until the actual board/header wiring is verified.
// Do not reuse audio, Ethernet, C6 SDIO, TF, touch/display or USB pins.
// Each HX711 module uses its own DOUT/SCK pair, channel A, gain 128.
namespace hx711_config {
struct Pins {
    int dout;
    int sck;
};
inline constexpr Pins kChannels[2] = {{-1, -1}, {-1, -1}};
}  // namespace hx711_config
