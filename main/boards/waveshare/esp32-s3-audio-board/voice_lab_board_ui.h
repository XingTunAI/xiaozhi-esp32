#ifndef WAVESHARE_VOICE_LAB_BOARD_UI_H_
#define WAVESHARE_VOICE_LAB_BOARD_UI_H_

#include <esp_log.h>

#include <atomic>
#include <functional>
#include <utility>

#include "application.h"
#include "board.h"
#include "button.h"
#include "display/display.h"
#include "led/circular_strip.h"
#include "voice_lab_client.h"
#include "voice_lab_prompts.h"
#include "wifi_manager.h"

// Customer indicators belong to this board. Voice-assistant listening and VAD
// never imply that Voice Lab is recording.
class WaveshareVoiceLabStrip : public CircularStrip {
public:
    WaveshareVoiceLabStrip(gpio_num_t gpio, uint16_t count)
        : CircularStrip(gpio, count, LED_STRIP_COLOR_COMPONENT_FMT_RGB) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            auto* strip = static_cast<WaveshareVoiceLabStrip*>(arg);
            // WiFi status getters take a mutex. Query them on the application
            // task instead of blocking the shared ESP timer task.
            strip->ScheduleRefresh();
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "vl_board_status";
        args.skip_unhandled_events = true;
        ESP_ERROR_CHECK(esp_timer_create(&args, &status_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(status_timer_, 250000));
    }

    ~WaveshareVoiceLabStrip() override {
        esp_timer_stop(status_timer_);
        esp_timer_delete(status_timer_);
    }

    void OnNetworkEvent(NetworkEvent event) {
        switch (event) {
            case NetworkEvent::Scanning:
            case NetworkEvent::Connecting:
                // Keep an outage visible while the automatic retry is running.
                if (network_.load() != NetworkStatus::Disconnected) {
                    network_.store(NetworkStatus::Connecting);
                }
                break;
            case NetworkEvent::Connected:
                network_.store(NetworkStatus::Connected);
                break;
            case NetworkEvent::Disconnected:
                if (network_.load() != NetworkStatus::Configuring) {
                    network_.store(NetworkStatus::Disconnected);
                }
                break;
            case NetworkEvent::WifiConfigModeEnter:
                network_.store(NetworkStatus::Configuring);
                break;
            case NetworkEvent::WifiConfigModeExit:
                network_.store(NetworkStatus::Connecting);
                break;
            default:
                return;
        }
        ScheduleRefresh();
    }

    void OnStateChanged() override { ScheduleRefresh(); }

private:
    enum class NetworkStatus { Connecting, Connected, Disconnected, Configuring };
    enum class Indicator { Unknown, Connecting, Configuring, Ready, Recording, Error };

    static const char* IndicatorName(Indicator state) {
        switch (state) {
            case Indicator::Connecting:
                return "connecting";
            case Indicator::Configuring:
                return "configuring";
            case Indicator::Ready:
                return "ready";
            case Indicator::Recording:
                return "recording";
            case Indicator::Error:
                return "error";
            default:
                return "unknown";
        }
    }

    std::atomic<NetworkStatus> network_{NetworkStatus::Connecting};
    std::atomic<Indicator> indicator_{Indicator::Unknown};
    std::atomic<bool> refresh_pending_{false};
    esp_timer_handle_t status_timer_ = nullptr;

    Indicator SelectIndicator() const {
        auto& client = VoiceLabClient::GetInstance();
        auto user_state = client.GetUserState();
        if (client.IsRecording()) {
            return Indicator::Recording;
        }
        auto state = Application::GetInstance().GetDeviceState();
        auto network = network_.load();
        if (WifiManager::GetInstance().IsConfigMode()) {
            return Indicator::Configuring;
        }
        if (state == kDeviceStateFatalError || network == NetworkStatus::Disconnected) {
            return Indicator::Error;
        }
        if (network != NetworkStatus::Connected) {
            return Indicator::Connecting;
        }
        if (user_state == VoiceLabClient::UserState::Error) {
            return Indicator::Error;
        }
        if (user_state == VoiceLabClient::UserState::Ready) {
            return Indicator::Ready;
        }
        return Indicator::Connecting;
    }

    void ScheduleRefresh() {
        if (refresh_pending_.exchange(true)) {
            return;
        }
        Application::GetInstance().Schedule([this]() {
            refresh_pending_.store(false);
            auto next = SelectIndicator();
            if (indicator_.exchange(next) == next) {
                return;
            }
            auto& client = VoiceLabClient::GetInstance();
            ESP_LOGI("WaveshareStatus", "indicator=%s recording=%d control_connected=%d",
                     IndicatorName(next), client.IsRecording(), client.IsConnected());
            // The board specifies its wire order in the constructor. All
            // effects below use semantic RGB and nonzero recording brightness.
            switch (next) {
                case Indicator::Configuring:
                    SetAllColor({24, 16, 0});
                    Blink({24, 16, 0}, 500);
                    break;
                case Indicator::Connecting:
                    SetAllColor({0, 0, 0});
                    Scroll({0, 0, 0}, {24, 16, 0}, 2, 150);
                    break;
                case Indicator::Ready:
                    SetAllColor({0, 4, 0});
                    break;
                case Indicator::Recording:
                    SetAllColor({0, 0, 32});
                    break;
                case Indicator::Error:
                    SetAllColor({32, 0, 0});
                    Blink({32, 0, 0}, 500);
                    break;
                case Indicator::Unknown:
                    break;
            }
        });
    }
};

class WaveshareVoiceLabButtons {
public:
    void Bind(Button& button, std::function<void()> enter_wifi) {
        enter_wifi_ = std::move(enter_wifi);
        button.OnPressDown([this]() { long_press_handled_.store(false); });
        button.OnLongPress([this]() {
            if (!long_press_handled_.exchange(true)) {
                Application::GetInstance().Schedule([this]() { RequestWifiConfig(); });
            }
        });
        button.OnClick([this]() {
            if (long_press_handled_.load()) {
                return;
            }
            Application::GetInstance().Schedule([this]() {
                if (WifiManager::GetInstance().IsConfigMode()) {
                    ShowHint("请用手机连接设备热点完成配网");
                    QueueVoiceLabPrompt(VoiceLabPrompt::WifiSetup);
                } else {
                    ShowHint("长按 BOOT 3 秒配网；K2 控制录音");
                }
            });
        });
    }

    // The expander scanner dispatches these methods on the application task.
    // Short presses do not change capture or play sound into a recording.
    void OnPrimaryClick() {
        auto& client = VoiceLabClient::GetInstance();
        if (!client.IsRecording())
            client.BeginCustomerBinding();
        ShowHint("未绑定时短按 K2 获取绑定码；长按 2 秒控制录音");
    }

    void OnPrimaryLongPress() {
        auto& client = VoiceLabClient::GetInstance();
        if (client.IsRecording()) {
            if (!action_pending_) {
                ShowHint("正在停止录音，请稍候");
                RunWorker(Action::Stop);
            }
            return;
        }
        auto state = Application::GetInstance().GetDeviceState();
        if (WifiManager::GetInstance().IsConfigMode()) {
            ShowHint("请先完成手机配网，再长按 K2 开始");
            QueueVoiceLabPrompt(VoiceLabPrompt::WifiSetup);
        } else if (state == kDeviceStateWifiConfiguring) {
            ShowHint("长按 BOOT 3 秒重新配网");
        } else if (state == kDeviceStateUnknown || state == kDeviceStateUpgrading ||
                   state == kDeviceStateFatalError) {
            ShowHint("设备当前无法开始录音，请稍后重试");
        } else if (action_pending_ ||
                   client.GetUserState() == VoiceLabClient::UserState::Requesting ||
                   client.GetUserState() == VoiceLabClient::UserState::Starting ||
                   client.GetUserState() == VoiceLabClient::UserState::Stopping) {
            ShowHint("正在处理，请等待设备确认");
        } else {
            // This asks the authenticated server for a new recording. A local
            // key never directly grants capture permission.
            RunWorker(Action::RequestStart);
        }
    }

private:
    enum class Action { Stop, Reconfigure, RequestStart };
    std::function<void()> enter_wifi_;
    std::atomic<bool> long_press_handled_{false};
    // Only the application task changes action scheduling. One worker handles
    // socket flushing; a long press during a stop is retained for completion.
    bool action_pending_ = false;
    bool wifi_requested_ = false;
    Action worker_action_ = Action::Stop;

    static void ShowHint(const char* text) {
        auto* display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->ShowNotification(text, 5000);
        }
    }

    void RequestWifiConfig() {
        auto state = Application::GetInstance().GetDeviceState();
        if (WifiManager::GetInstance().IsConfigMode()) {
            ShowHint("请用手机连接设备热点完成配网");
            QueueVoiceLabPrompt(VoiceLabPrompt::WifiSetup);
            return;
        }
        if (state == kDeviceStateUpgrading || state == kDeviceStateFatalError ||
            state == kDeviceStateUnknown) {
            ShowHint("设备当前无法配网，请稍后重试");
            return;
        }
        wifi_requested_ = true;
        ShowHint("正在进入配网模式，设备绑定会保留");
        if (!action_pending_) {
            RunWorker(Action::Reconfigure);
        }
    }

    void RunWorker(Action action) {
        action_pending_ = true;
        worker_action_ = action;
        if (action == Action::Reconfigure) {
            wifi_requested_ = false;
        }
        BaseType_t result = xTaskCreate(
            [](void* arg) {
                auto* controls = static_cast<WaveshareVoiceLabButtons*>(arg);
                const auto action = controls->worker_action_;
                auto& client = VoiceLabClient::GetInstance();
                const bool success = action == Action::RequestStart ? client.RequestStartRecording()
                                                                    : client.StopRecording();
                if (action == Action::Reconfigure) {
                    client.StopPlayback();
                    client.Disconnect();
                }
                Application::GetInstance().Schedule([controls, action, success]() {
                    controls->action_pending_ = false;
                    if (action == Action::Reconfigure) {
                        controls->wifi_requested_ = false;
                        controls->enter_wifi_();
                    } else if (controls->wifi_requested_) {
                        controls->RunWorker(Action::Reconfigure);
                    } else if (action == Action::RequestStart) {
                        if (!success) {
                            ShowHint("暂时无法开始，请确认设备在线并查看网页");
                        } else if (!VoiceLabClient::GetInstance().IsRecording()) {
                            ShowHint("已请求开始，请等待设备确认");
                        }
                    } else if (success && !VoiceLabClient::GetInstance().IsRecording()) {
                        ShowHint("录音已停止；长按 K2 或在网页开始下一次录音");
                    } else if (!success) {
                        ShowHint("录音已中断，请在网页检查结果");
                    }
                });
                vTaskDelete(nullptr);
            },
            "vl_board_action", 6144, this, 2, nullptr);
        if (result != pdPASS) {
            action_pending_ = false;
            wifi_requested_ = false;
            ShowHint("操作未完成，请稍后重试");
        }
    }
};

#endif  // WAVESHARE_VOICE_LAB_BOARD_UI_H_
