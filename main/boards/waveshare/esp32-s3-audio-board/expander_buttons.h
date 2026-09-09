#ifndef WAVESHARE_EXPANDER_BUTTONS_H_
#define WAVESHARE_EXPANDER_BUTTONS_H_

#include <esp_io_expander.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>

#include "application.h"

// Existing on-board keys, verified against the official revision 1.1 schematic:
// https://files.waveshare.com/wiki/ESP32-S3-AUDIO-Board/ESP32-S3-AUDIO-Board_1.1.pdf
// Key1/Key2/Key3 -> Extend_IO9/10/11 (TCA9555 P11/P12/P13), respectively.
// R7/R44/R77 pull them up to 3V3; pressing a key connects it to GND.
// Official factory_01/main/button_driver/button_driver.c in the demo below
// independently confirms the three low-active inputs. Key4 is GPIO0/BOOT and
// Key5 is RESET; neither is part of this expander driver.
// https://files.waveshare.net/wiki/ESP32-S3-AUDIO-Board/ESP32-S3-AUDIO-Board-Demo.zip
class WaveshareExpanderButtons {
public:
    enum class Key { K1, K2, K3 };
    using Callback = std::function<void(Key)>;

    WaveshareExpanderButtons() = default;
    WaveshareExpanderButtons(const WaveshareExpanderButtons&) = delete;
    WaveshareExpanderButtons& operator=(const WaveshareExpanderButtons&) = delete;

    ~WaveshareExpanderButtons() {
        if (state_) {
            state_->stopping.store(true);
        }
    }

    bool Initialize(esp_io_expander_handle_t expander, Callback on_click, Callback on_long_press) {
        if (state_ || expander == nullptr) {
            return false;
        }
        // Set only the existing key pins to input. Preserve LCD/camera/PA and
        // USB mux outputs on the same expander; do not reset the TCA9555.
        if (esp_io_expander_set_dir(expander, kKeyMask, IO_EXPANDER_INPUT) != ESP_OK) {
            return false;
        }
        state_ = std::make_shared<State>();
        state_->expander = expander;
        state_->on_click = std::move(on_click);
        state_->on_long_press = std::move(on_long_press);
        auto* task_state = new std::shared_ptr<State>(state_);
        const auto created = xTaskCreate(
            [](void* arg) {
                auto* holder = static_cast<std::shared_ptr<State>*>(arg);
                auto state = std::move(*holder);
                delete holder;
                Poll(state);
                state.reset();
                vTaskDelete(nullptr);
            },
            "waveshare_keys", 3072, task_state, 2, nullptr);
        if (created != pdPASS) {
            delete task_state;
            state_.reset();
            return false;
        }
        return true;
    }

private:
    static constexpr uint32_t kKeyMask =
        IO_EXPANDER_PIN_NUM_9 | IO_EXPANDER_PIN_NUM_10 | IO_EXPANDER_PIN_NUM_11;
    static constexpr int64_t kLongPressUs = 3000000;
    static constexpr int64_t kMaximumSampleGapUs = 100000;
    static constexpr unsigned kDebounceSamples = 3;

    struct KeyState {
        bool candidate = false;
        bool pressed = false;
        bool armed = false;
        bool long_sent = false;
        unsigned samples = 0;
        int64_t pressed_at = 0;
    };

    struct State {
        esp_io_expander_handle_t expander = nullptr;
        Callback on_click;
        Callback on_long_press;
        std::array<KeyState, 3> keys{};
        std::array<std::atomic<bool>, 3> event_pending{};
        std::atomic<bool> stopping{false};
    };

    std::shared_ptr<State> state_;

    static void PostEvent(const std::shared_ptr<State>& state, size_t index, bool long_press) {
        // A slow application can retain at most one action per key. Never let
        // bouncing or repeated presses build an unbounded application queue.
        if (state->event_pending[index].exchange(true)) {
            return;
        }
        Application::GetInstance().Schedule([state, index, long_press]() {
            state->event_pending[index].store(false);
            if (state->stopping.load()) {
                return;
            }
            auto& callback = long_press ? state->on_long_press : state->on_click;
            if (callback) {
                callback(static_cast<Key>(index));
            }
        });
    }

    static void Poll(const std::shared_ptr<State>& state) {
        int64_t last_sample = 0;
        int64_t last_error = 0;
        while (!state->stopping.load()) {
            uint32_t levels = kKeyMask;
            const auto result = esp_io_expander_get_level(state->expander, kKeyMask, &levels);
            const auto now = esp_timer_get_time();
            if (result != ESP_OK || (last_sample != 0 && now - last_sample > kMaximumSampleGapUs)) {
                // Cancel the gesture on a failed/stalled I2C read. Rearm only
                // after a stable release, so recovery cannot manufacture a
                // click/start request or turn an old press into a long press.
                state->keys = {};
                if (result != ESP_OK && (last_error == 0 || now - last_error >= 1000000)) {
                    ESP_LOGW("WaveshareKeys", "Key scan failed: %s", esp_err_to_name(result));
                    last_error = now;
                }
            } else {
                for (size_t index = 0; index < state->keys.size(); ++index) {
                    auto& key = state->keys[index];
                    const bool pressed = (levels & (1U << (9 + index))) == 0;
                    if (pressed != key.candidate) {
                        key.candidate = pressed;
                        key.samples = 1;
                    } else if (key.samples < kDebounceSamples) {
                        ++key.samples;
                    }
                    if (key.samples < kDebounceSamples) {
                        continue;
                    }
                    if (key.pressed != key.candidate) {
                        key.pressed = key.candidate;
                        if (key.pressed) {
                            key.pressed_at = now;
                            key.long_sent = false;
                        } else {
                            if (key.armed && !key.long_sent && key.pressed_at != 0) {
                                PostEvent(state, index, now - key.pressed_at >= kLongPressUs);
                            }
                            key.pressed_at = 0;
                        }
                    }
                    if (!key.pressed) {
                        key.armed = true;
                    } else if (key.armed && !key.long_sent &&
                               now - key.pressed_at >= kLongPressUs) {
                        key.long_sent = true;
                        PostEvent(state, index, true);
                    }
                }
            }
            last_sample = now;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
};

#endif  // WAVESHARE_EXPANDER_BUTTONS_H_
