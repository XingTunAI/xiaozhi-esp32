#include "board_scale.h"
#include "hx711_config.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <sstream>

namespace {
constexpr char TAG[] = "P4Scale";
struct Channel {
    BoardScaleStatus status;
    int32_t simulated_raw = 0;
    bool simulated_connected = true;
    int64_t last_poll_ms = 0;
};
Channel channels[2];
std::mutex mutex;
TaskHandle_t worker = nullptr;
portMUX_TYPE clock_lock = portMUX_INITIALIZER_UNLOCKED;

bool ValidPins(unsigned index) {
    const auto pins = hx711_config::kChannels[index];
    if (!GPIO_IS_VALID_GPIO(pins.dout) || !GPIO_IS_VALID_OUTPUT_GPIO(pins.sck) ||
        pins.dout == pins.sck)
        return false;
    const auto other = hx711_config::kChannels[1 - index];
    return pins.dout != other.dout && pins.dout != other.sck && pins.sck != other.dout &&
           pins.sck != other.sck;
}

// No conversion wait here. Protect each short clock-high interval against task preemption.
// Data sheet: 24 data clocks + one gain-select clock => channel A / gain 128.
bool ReadRaw(unsigned index, int32_t& raw) {
    const auto pins = hx711_config::kChannels[index];
    const auto dout = static_cast<gpio_num_t>(pins.dout);
    const auto sck = static_cast<gpio_num_t>(pins.sck);
    if (gpio_get_level(dout))
        return false;
    uint32_t bits = 0;
    esp_rom_delay_us(1);
    for (unsigned bit = 0; bit < 25; ++bit) {
        portENTER_CRITICAL(&clock_lock);
        gpio_set_level(sck, 1);
        esp_rom_delay_us(1);
        if (bit < 24)
            bits = (bits << 1) | gpio_get_level(dout);
        gpio_set_level(sck, 0);
        portEXIT_CRITICAL(&clock_lock);
        esp_rom_delay_us(1);
    }
    // DOUT must return high after clock 25; catches a disconnected/stuck-low wire.
    if (!gpio_get_level(dout))
        return false;
    raw = ScaleState::Decode(bits);
    return true;
}

void Poll(void*) {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            const int64_t now = esp_timer_get_time() / 1000;
            for (unsigned i = 0; i < 2; ++i) {
                auto& c = channels[i];
                auto& s = c.status.measurement;
                if (c.status.mode == BoardScaleMode::Off)
                    continue;
                // Common 10 Hz processing cadence supports RATE=10 or RATE=80 modules.
                if (now - c.last_poll_ms >= 100) {
                    c.last_poll_ms = now;
                    int32_t raw = c.simulated_raw;
                    const bool ready = c.status.mode == BoardScaleMode::Simulated
                                           ? c.simulated_connected
                                           : ReadRaw(i, raw);
                    if (ready) {
                        const auto previous = s.event_sequence;
                        s.Sample(raw, now);
                        if (s.event_sequence != previous)
                            ESP_LOGI(TAG,
                                     "channel=%u simulated=%d event=%s sequence=%lu grams=%.2f",
                                     i + 1, c.status.mode == BoardScaleMode::Simulated,
                                     s.event == ScaleState::Event::Placed ? "placed" : "removed",
                                     static_cast<unsigned long>(s.event_sequence), s.grams);
                    }
                }
                if (s.connected && now - s.timestamp_ms > 1500) {
                    s.Disconnect();
                    ESP_LOGW(TAG, "channel=%u timeout", i + 1);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool SetMode(unsigned index, BoardScaleMode mode) {
    auto& c = channels[index];
    if (c.status.mode == mode)
        return true;
    if (mode == BoardScaleMode::Hardware && !ValidPins(index))
        return false;
    if (!worker && mode != BoardScaleMode::Off &&
        xTaskCreate(Poll, "hx711", 4096, nullptr, 2, &worker) != pdPASS)
        return false;
    if (c.status.mode == BoardScaleMode::Hardware) {
        const auto pins = hx711_config::kChannels[index];
        gpio_set_level(static_cast<gpio_num_t>(pins.sck), 0);
        gpio_reset_pin(static_cast<gpio_num_t>(pins.sck));
        gpio_reset_pin(static_cast<gpio_num_t>(pins.dout));
    }
    c = Channel{};
    if (mode == BoardScaleMode::Hardware) {
        const auto pins = hx711_config::kChannels[index];
        gpio_config_t config{};
        config.pin_bit_mask = 1ULL << pins.sck;
        config.mode = GPIO_MODE_OUTPUT;
        gpio_set_level(static_cast<gpio_num_t>(pins.sck), 0);
        auto err = gpio_config(&config);
        if (err == ESP_OK) {
            config.pin_bit_mask = 1ULL << pins.dout;
            config.mode = GPIO_MODE_INPUT;
            config.pull_up_en = GPIO_PULLUP_ENABLE;
            err = gpio_config(&config);
        }
        if (err != ESP_OK) {
            gpio_reset_pin(static_cast<gpio_num_t>(pins.sck));
            gpio_reset_pin(static_cast<gpio_num_t>(pins.dout));
            return false;
        }
    } else if (mode == BoardScaleMode::Simulated) {
        c.status.measurement.ConfigureSimulation();
    }
    c.status.mode = mode;
    return true;
}

void PrintStatus(unsigned index) {
    const auto& c = channels[index];
    const auto& s = c.status.measurement;
    const char* mode = c.status.mode == BoardScaleMode::Off         ? "off"
                       : c.status.mode == BoardScaleMode::Simulated ? "simulated"
                                                                    : "hardware";
    ESP_LOGI(TAG,
             "channel=%u mode=%s connected=%d calibrated=%d stable=%d overload=%d "
             "raw=%ld grams=%.2f valid=%d timestamp_ms=%lld sequence=%lu",
             index + 1, mode, s.connected, s.calibrated, s.stable, s.overload,
             static_cast<long>(s.raw), s.grams, s.connected && s.calibrated && !s.overload,
             static_cast<long long>(s.timestamp_ms), static_cast<unsigned long>(s.event_sequence));
}
}  // namespace

BoardScaleStatus GetBoardScaleStatus(unsigned channel) {
    std::lock_guard<std::mutex> lock(mutex);
    if (channel >= 2)
        return {};
    auto result = channels[channel].status;
    if (result.measurement.connected &&
        esp_timer_get_time() / 1000 - result.measurement.timestamp_ms > 1500)
        result.measurement.Disconnect();
    return result;
}

bool HandleBoardScaleCommand(const std::string& command) {
    if (command != "p4 scale" && command.compare(0, 9, "p4 scale ") != 0)
        return false;
    std::istringstream input(command.substr(8));
    unsigned channel = 0;
    std::string action, extra;
    double value = 0;
    bool valid = static_cast<bool>(input >> channel >> action) && channel >= 1 && channel <= 2;
    const bool needs_value = action == "weight" || action == "calibrate";
    if (valid && needs_value)
        valid = static_cast<bool>(input >> value) && std::isfinite(value);
    if (!valid || (input >> extra)) {
        ESP_LOGI(TAG,
                 "p4 scale <1|2> <status|sim|hardware|off|tare|disconnect|reconnect> OR "
                 "p4 scale <1|2> <weight|calibrate> <grams>");
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const unsigned index = channel - 1;
    auto& c = channels[index];
    auto& s = c.status.measurement;
    if (s.connected && esp_timer_get_time() / 1000 - s.timestamp_ms > 1500)
        s.Disconnect();
    bool ok = false;
    if (action == "status")
        ok = true;
    else if (action == "off" || action == "sim" || action == "hardware")
        ok = SetMode(index, action == "off"   ? BoardScaleMode::Off
                            : action == "sim" ? BoardScaleMode::Simulated
                                              : BoardScaleMode::Hardware);
    else if (action == "weight" && c.status.mode == BoardScaleMode::Simulated && value >= -1500 &&
             value <= 1500) {
        c.simulated_raw = static_cast<int32_t>(std::lround(value * 1000));
        ok = true;
    } else if ((action == "disconnect" || action == "reconnect") &&
               c.status.mode == BoardScaleMode::Simulated) {
        c.simulated_connected = action == "reconnect";
        s.Disconnect();
        ok = true;
    } else if (action == "tare")
        ok = s.Tare();
    else if (action == "calibrate" && c.status.mode == BoardScaleMode::Hardware)
        ok = s.Calibrate(value);
    ESP_LOGI(TAG, "channel=%u action=%s result=%s", channel, action.c_str(),
             ok ? "ok" : "rejected");
    PrintStatus(index);
    return true;
}
