#include "boards/waveshare/esp32-p4-wifi6-dev-kit-b/scale_state.h"

constexpr void Feed(ScaleState& s, int32_t raw, int64_t& now, unsigned count = 8) {
    for (unsigned i = 0; i < count; ++i) {
        now += 100;
        s.Sample(raw, now);
    }
}

constexpr bool PlacementAndRemoval() {
    ScaleState s;
    s.ConfigureSimulation();
    int64_t now = 0;
    Feed(s, 0, now);
    if (!s.stable || s.event_sequence != 0)
        return false;
    Feed(s, 100000, now);
    if (!s.stable || s.grams != 100 || s.event != ScaleState::Event::Placed ||
        s.event_sequence != 1)
        return false;
    Feed(s, 100000, now, 30);
    Feed(s, 3000, now);  // Hysteresis: still present at 3g.
    if (s.event_sequence != 1)
        return false;
    Feed(s, 0, now);
    return s.stable && !s.present && s.event == ScaleState::Event::Removed && s.event_sequence == 2;
}

constexpr bool NoiseAndOverload() {
    ScaleState s;
    s.ConfigureSimulation();
    int64_t now = 0;
    Feed(s, 0, now);
    Feed(s, 100000, now, 1);  // A transient must not trigger placement.
    Feed(s, 0, now);
    if (s.event_sequence != 0)
        return false;
    for (int i = 0; i < 30; ++i)
        Feed(s, i % 2 ? 10000 : 20000, now, 1);
    if (s.stable || s.event_sequence != 0)
        return false;
    Feed(s, 800000, now);
    if (!s.overload || s.stable || s.event_sequence != 0)
        return false;
    Feed(s, 0, now);
    Feed(s, 8388607, now, 1);
    return s.overload && !s.Tare();
}

constexpr bool CalibrationAndTare() {
    ScaleState s;
    int64_t now = 0;
    Feed(s, 50000, now);
    if (s.calibrated || s.Calibrate(100) || !s.Tare())
        return false;
    Feed(s, 50000, now);
    if (s.Calibrate(100))  // No measurable reference load.
        return false;
    Feed(s, -50000, now);  // Reversed bridge polarity is supported.
    if (s.Calibrate(0) || s.Calibrate(1e-300) || s.Calibrate(751) || !s.Calibrate(100))
        return false;
    Feed(s, -50000, now);
    if (!s.stable || s.grams != 100 || s.event_sequence != 0)
        return false;
    if (!s.Tare())
        return false;
    Feed(s, -50000, now);
    return s.stable && s.grams == 0 && s.event_sequence == 0;
}

constexpr bool DisconnectAndIsolation() {
    ScaleState a, b;
    a.ConfigureSimulation();
    b.ConfigureSimulation();
    int64_t now = 0;
    Feed(a, 0, now);
    Feed(a, 100000, now);
    a.Disconnect();
    if (a.connected || a.stable || a.Tare())
        return false;
    Feed(a, 0, now);
    if (a.event_sequence != 1 || a.event != ScaleState::Event::None || b.connected)
        return false;  // Reconnection creates a baseline, never a phantom removal.
    now += 2000;
    Feed(a, 100000, now, 1);
    return !a.stable && a.event_sequence == 1;
}

constexpr bool FastSamplesCannotSettle() {
    ScaleState s;
    s.ConfigureSimulation();
    for (int i = 0; i < 20; ++i)
        s.Sample(0, i);
    return !s.stable && !s.Tare();
}

static_assert(ScaleState::Decode(0x000000) == 0);
static_assert(ScaleState::Decode(0x7fffff) == 8388607);
static_assert(ScaleState::Decode(0x800000) == -8388608);
static_assert(ScaleState::Decode(0xffffff) == -1);
static_assert(PlacementAndRemoval());
static_assert(NoiseAndOverload());
static_assert(CalibrationAndTare());
static_assert(DisconnectAndIsolation());
static_assert(FastSamplesCannotSettle());
