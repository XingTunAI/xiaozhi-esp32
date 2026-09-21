#pragma once

#include <cstddef>
#include <cstdint>

// Pure processing shared by physical HX711 samples and the simulator.
// Callers serialize access. No allocations, GPIO, logging or application mutations here.
class ScaleState {
public:
    enum class Event { None, Placed, Removed };
    static constexpr size_t kWindow = 8;
    static constexpr double kCapacityGrams = 750;
    static constexpr double Abs(double n) { return n < 0 ? -n : n; }
    static constexpr int32_t Decode(uint32_t bits) {
        return (bits & 0x800000) ? static_cast<int32_t>(bits & 0xffffff) - 0x1000000
                                 : static_cast<int32_t>(bits & 0xffffff);
    }

    constexpr void Disconnect() {
        connected = stable = raw_stable = false;
        count_ = next_ = 0;
        baseline_ = false;
        event = Event::None;
    }

    constexpr void Sample(int32_t value, int64_t now_ms) {
        if (connected && now_ms - timestamp_ms > 1500)
            Disconnect();
        connected = true;
        timestamp_ms = now_ms;
        raw = value;
        samples_[next_] = value;
        times_[next_] = now_ms;
        next_ = (next_ + 1) % kWindow;
        if (count_ < kWindow)
            ++count_;
        double sum = 0;
        int32_t lo = value, hi = value;
        for (size_t i = 0; i < count_; ++i) {
            sum += samples_[i];
            if (samples_[i] < lo)
                lo = samples_[i];
            if (samples_[i] > hi)
                hi = samples_[i];
        }
        mean_raw_ = sum / count_;
        const bool full = count_ == kWindow && now_ms - times_[next_] >= 700;
        const bool saturated = lo == -8388608 || hi == 8388607;
        raw_stable = full && !saturated && static_cast<double>(hi) - lo <= 1000;
        grams = calibrated ? (mean_raw_ - offset_) / counts_per_gram_ : 0;
        // Include the current sample so averaging cannot hide overload/saturation.
        overload = saturated ||
                   (calibrated && (Abs(grams) > kCapacityGrams ||
                                   Abs((value - offset_) / counts_per_gram_) > kCapacityGrams));
        stable = calibrated && full && !overload &&
                 (static_cast<double>(hi) - lo) / Abs(counts_per_gram_) <= 1.0;
        if (!stable)
            return;
        if (!baseline_) {
            present = grams >= 5;
            baseline_ = true;
            return;
        }
        if (!present && grams >= 5) {
            present = true;
            event = Event::Placed;
            ++event_sequence;
        } else if (present && Abs(grams) <= 2) {
            present = false;
            event = Event::Removed;
            ++event_sequence;
        }
    }

    constexpr bool Tare() {
        if (!connected || overload || !(calibrated ? stable : raw_stable))
            return false;
        offset_ = mean_raw_;
        tared_ = true;
        Disconnect();
        return true;
    }

    constexpr bool Calibrate(double known_grams) {
        if (!connected || raw == -8388608 || raw == 8388607 || !raw_stable || !tared_ ||
            !(known_grams >= 1) || known_grams > kCapacityGrams || Abs(mean_raw_ - offset_) < 100)
            return false;
        counts_per_gram_ = (mean_raw_ - offset_) / known_grams;
        calibrated = true;
        Disconnect();
        return true;
    }

    constexpr void ConfigureSimulation() {
        *this = ScaleState{};
        counts_per_gram_ = 1000;
        calibrated = true;
    }

    bool connected = false;
    bool calibrated = false;
    bool stable = false;
    bool raw_stable = false;
    bool overload = false;
    bool present = false;
    double grams = 0;
    int32_t raw = 0;
    int64_t timestamp_ms = 0;
    Event event = Event::None;
    uint32_t event_sequence = 0;

private:
    int32_t samples_[kWindow]{};
    int64_t times_[kWindow]{};
    size_t next_ = 0, count_ = 0;
    double mean_raw_ = 0, offset_ = 0, counts_per_gram_ = 1;
    bool tared_ = false, baseline_ = false;
};
