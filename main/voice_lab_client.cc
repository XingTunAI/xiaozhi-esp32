#include "voice_lab_client.h"
#include "voice_lab_prompts.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "led/led.h"
#include "mcp_server.h"
#include "settings.h"
#include "system_info.h"

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <http.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#define TAG "VoiceLab"

#ifndef CONFIG_VOICE_LAB_AUTO_CONNECT
#define CONFIG_VOICE_LAB_AUTO_CONNECT 1
#endif

#ifndef CONFIG_VOICE_LAB_SERVER_HOST
#define CONFIG_VOICE_LAB_SERVER_HOST "voice-lab.cloud"
#endif

#ifndef CONFIG_VOICE_LAB_SERVER_PORT
#define CONFIG_VOICE_LAB_SERVER_PORT 1883
#endif

#ifndef CONFIG_VOICE_LAB_SERVER_TLS
#define CONFIG_VOICE_LAB_SERVER_TLS 1
#endif

#ifndef CONFIG_VOICE_LAB_SERVER_URL
#define CONFIG_VOICE_LAB_SERVER_URL ""
#endif

#ifndef CONFIG_VOICE_LAB_DEVICE_KEY
#define CONFIG_VOICE_LAB_DEVICE_KEY ""
#endif

#ifndef CONFIG_VOICE_LAB_DEVICE_TOKEN
#ifdef CONFIG_VOICE_LAB_TOKEN
#define CONFIG_VOICE_LAB_DEVICE_TOKEN CONFIG_VOICE_LAB_TOKEN
#else
#define CONFIG_VOICE_LAB_DEVICE_TOKEN ""
#endif
#endif

#ifndef CONFIG_VOICE_LAB_HARDWARE_PROFILE
#define CONFIG_VOICE_LAB_HARDWARE_PROFILE "waveshare-esp32s3-audio-board"
#endif

#ifndef CONFIG_VOICE_LAB_AUDIO_FRONTEND
#define CONFIG_VOICE_LAB_AUDIO_FRONTEND "waveshare-es7210"
#endif

namespace {

constexpr const char* kControlSchemaVersion = "voice-lab-device-control-v1";
constexpr const char* kAudioProtocol = "device-audio-v2";
constexpr const char* kCapabilitySchemaVersion = "1";
constexpr const char* kAudioFrontendFirmware = "xiaozhi-es7210";
constexpr size_t kPcmPacketHeaderSize = 40;
constexpr int kControlHeartbeatIntervalMs = 5000;
constexpr int kEnrollmentHttpTimeoutMs = 10000;
constexpr int kTlsTimeWaitMs = 15000;
// Administrator capture continues until stopped. Leased sessions still require
// timely renewals and enforce any positive total duration granted by the server.
constexpr int64_t kControlSilenceLimitUs = 15LL * 1000000;
constexpr int kAudioAcceptTimeoutMs = 5000;
constexpr int kCaptureTransitionAckTimeoutMs = 10000;
constexpr int kAudioDrainTimeoutMs = 10000;
constexpr size_t kMaxUnacknowledgedPackets = 8;  // Four seconds of PCM, bounded.
std::atomic<bool> g_usb_provisioning_task_started{false};
std::atomic<bool> g_sntp_started{false};

bool JsonInteger(const cJSON* value, int64_t minimum, int64_t maximum) {
    return cJSON_IsNumber(value) && std::isfinite(value->valuedouble) &&
           value->valuedouble >= minimum && value->valuedouble <= maximum &&
           value->valuedouble == std::floor(value->valuedouble);
}

bool JsonText(const cJSON* value, const char* expected) {
    return cJSON_IsString(value) && strcmp(value->valuestring, expected) == 0;
}

std::string TrimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string DeriveDeviceKeyFromMac() {
    std::string mac = SystemInfo::GetMacAddress();
    std::string key = "waveshare-esp32s3-audio-";
    for (char ch : mac) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return key;
}

void AppendLe32(std::string& out, uint32_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
    out.push_back(static_cast<char>((value >> 16) & 0xff));
    out.push_back(static_cast<char>((value >> 24) & 0xff));
}

void AppendLe64(std::string& out, uint64_t value) {
    AppendLe32(out, static_cast<uint32_t>(value & 0xffffffffULL));
    AppendLe32(out, static_cast<uint32_t>((value >> 32) & 0xffffffffULL));
}

bool IsHttpUrl(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}

std::string NormalizeEnrollmentCode(std::string code) {
    std::string normalized;
    for (char ch : code) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
    }
    if (normalized.size() == 8) {
        normalized.insert(4, "-");
    }
    return normalized;
}

std::string TrimAsciiWhitespace(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

bool ConsumePrefix(std::string& value, const char* prefix) {
    const size_t len = strlen(prefix);
    if (value.compare(0, len, prefix) != 0) {
        return false;
    }
    value = TrimAsciiWhitespace(value.substr(len));
    return true;
}

bool IsSystemTimeValidForTls() {
    time_t now = 0;
    time(&now);
    return now >= 1704067200;  // 2024-01-01 UTC.
}

bool EnsureSystemTimeForTls(bool tls_enabled) {
    if (!tls_enabled || IsSystemTimeValidForTls()) {
        return true;
    }

    bool expected = false;
    if (g_sntp_started.compare_exchange_strong(expected, true)) {
        ESP_LOGI(TAG, "System time is not set; starting SNTP before Voice Lab TLS");
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_init();
    } else if (esp_sntp_enabled()) {
        ESP_LOGI(TAG, "System time is not set; waiting for existing SNTP sync");
    }

    const int step_ms = 500;
    for (int elapsed_ms = 0; elapsed_ms < kTlsTimeWaitMs; elapsed_ms += step_ms) {
        if (IsSystemTimeValidForTls()) {
            time_t now = 0;
            time(&now);
            ESP_LOGI(TAG, "System time synchronized for Voice Lab TLS: %lld",
                     static_cast<long long>(now));
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }

    ESP_LOGW(TAG, "System time is still not valid after SNTP wait; postpone Voice Lab TLS connect");
    return false;
}

}  // namespace

VoiceLabClient::~VoiceLabClient() { Disconnect(); }

bool VoiceLabClient::IsAutoConnectEnabled() const {
    Settings settings("voice_lab", false);
    return settings.GetBool("auto_connect", CONFIG_VOICE_LAB_AUTO_CONNECT);
}

std::string VoiceLabClient::GetConfiguredHost() const {
    Settings settings("voice_lab", false);
    auto host = settings.GetString("server_host", "");
    if (host.empty()) {
        host = CONFIG_VOICE_LAB_SERVER_HOST;
    }
    return host;
}

int VoiceLabClient::GetConfiguredPort() const {
    Settings settings("voice_lab", false);
    int port = settings.GetInt("server_port", 0);
    return port > 0 ? port : CONFIG_VOICE_LAB_SERVER_PORT;
}

bool VoiceLabClient::IsTlsEnabled() const {
    Settings settings("voice_lab", false);
    return settings.GetBool("server_tls", CONFIG_VOICE_LAB_SERVER_TLS);
}

std::string VoiceLabClient::GetConfiguredServerUrl() const {
    auto scheme = IsTlsEnabled() ? "https://" : "http://";
    return std::string(scheme) + GetConfiguredHost() + ":" + std::to_string(GetConfiguredPort());
}

std::string VoiceLabClient::GetDeviceKey() const {
    Settings settings("voice_lab", false);
    auto device_key = settings.GetString("device_key", CONFIG_VOICE_LAB_DEVICE_KEY);
    if (device_key.empty()) {
        device_key = DeriveDeviceKeyFromMac();
    }
    return device_key;
}

std::string VoiceLabClient::GetDeviceToken() const {
    Settings settings("voice_lab", false);
    auto token = settings.GetString("device_token", "");
    if (token.empty()) {
        token = settings.GetString("token", CONFIG_VOICE_LAB_DEVICE_TOKEN);
    }
    return token;
}

std::string VoiceLabClient::GetEnrollmentCode() const {
    Settings settings("voice_lab", false);
    return settings.GetString("enrollment_code", "");
}

std::string VoiceLabClient::GetHardwareProfile() const {
    Settings settings("voice_lab", false);
    auto value = settings.GetString("hw_profile", CONFIG_VOICE_LAB_HARDWARE_PROFILE);
    return value.empty() ? CONFIG_VOICE_LAB_HARDWARE_PROFILE : value;
}

std::string VoiceLabClient::GetAudioFrontend() const {
    Settings settings("voice_lab", false);
    auto value = settings.GetString("audio_frontend", CONFIG_VOICE_LAB_AUDIO_FRONTEND);
    return value.empty() ? CONFIG_VOICE_LAB_AUDIO_FRONTEND : value;
}

std::string VoiceLabClient::BuildHttpUrl(const std::string& path) const {
    return std::string(IsTlsEnabled() ? "https://" : "http://") + GetConfiguredHost() + ":" +
           std::to_string(GetConfiguredPort()) + path;
}

std::string VoiceLabClient::BuildWebsocketUrl(const std::string& path) const {
    return std::string(IsTlsEnabled() ? "wss://" : "ws://") + GetConfiguredHost() + ":" +
           std::to_string(GetConfiguredPort()) + path;
}

bool VoiceLabClient::PairWithEnrollmentCode(const std::string& enrollment_code) {
    auto normalized_code = NormalizeEnrollmentCode(enrollment_code);
    if (normalized_code.empty()) {
        ESP_LOGW(TAG, "Voice Lab enrollment code is empty");
        return false;
    }

    {
        Settings settings("voice_lab", true);
        settings.SetString("enrollment_code", normalized_code);
        settings.EraseKey("device_token");
        settings.EraseKey("token");
    }
    return EnrollIfNeeded();
}

void VoiceLabClient::ClearPairing() {
    Disconnect();
    Settings settings("voice_lab", true);
    settings.EraseKey("device_token");
    settings.EraseKey("token");
    settings.EraseKey("enrollment_code");
    ESP_LOGI(TAG, "Voice Lab pairing state cleared from NVS");
}

void VoiceLabClient::ResetVoiceLabSettings() {
    Disconnect();
    SsidManager::GetInstance().Clear();

    Settings settings("voice_lab", true);
    settings.EraseKey("server_url");
    settings.EraseKey("server_host");
    settings.EraseKey("server_port");
    settings.EraseKey("server_tls");
    settings.EraseKey("device_key");
    settings.EraseKey("device_token");
    settings.EraseKey("token");
    settings.EraseKey("enrollment_code");
    settings.EraseKey("hw_profile");
    settings.EraseKey("audio_frontend");
    settings.EraseKey("auto_connect");
    ESP_LOGI(TAG, "Voice Lab settings and saved Wi-Fi credentials reset from NVS");
}

bool VoiceLabClient::EnrollIfNeeded() {
    auto existing_token = GetDeviceToken();
    if (!existing_token.empty()) {
        return true;
    }

    auto enrollment_code = NormalizeEnrollmentCode(GetEnrollmentCode());
    if (enrollment_code.empty()) {
        ESP_LOGW(TAG, "Voice Lab device is not paired; generate an enrollment code in the console");
        return false;
    }
    if (!EnsureSystemTimeForTls(IsTlsEnabled())) {
        return false;
    }

    auto device_key = GetDeviceKey();
    auto host = GetConfiguredHost();
    auto port = GetConfiguredPort();
    auto tls = IsTlsEnabled();
    if (!tls && (port == 443 || port == 1883)) {
        ESP_LOGE(TAG,
                 "Invalid Voice Lab config: NoTls is set on a TLS port. Use voice-lab.cloud:1883 "
                 "tls=true or 106.55.21.5:18100 tls=false");
        return false;
    }
    auto url = BuildHttpUrl("/api/v1/devices/" + device_key + "/enroll");
    ESP_LOGI(TAG, "Pairing Voice Lab device %s with enrollment code", device_key.c_str());

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "code", enrollment_code.c_str());
    auto body = PrintJson(root);
    cJSON_Delete(root);

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(2);
    http->SetTimeout(kEnrollmentHttpTimeoutMs);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Cache-Control", "no-store");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid());
    http->SetContent(std::move(body));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Voice Lab enrollment HTTP open failed, code=%d", http->GetLastError());
        return false;
    }

    int status_code = http->GetStatusCode();
    auto response = http->ReadAll();
    http->Close();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Voice Lab enrollment failed, status=%d", status_code);
        if (!response.empty()) {
            ESP_LOGW(TAG, "Voice Lab enrollment error response: %.*s",
                     static_cast<int>(std::min<size_t>(response.size(), 160)), response.c_str());
        }
        return false;
    }

    cJSON* response_root = cJSON_Parse(response.c_str());
    if (response_root == nullptr) {
        ESP_LOGE(TAG, "Voice Lab enrollment response is not JSON");
        return false;
    }
    auto response_device_key = cJSON_GetObjectItem(response_root, "deviceKey");
    auto response_token = cJSON_GetObjectItem(response_root, "deviceToken");
    bool ok = cJSON_IsString(response_device_key) &&
              device_key == response_device_key->valuestring && cJSON_IsString(response_token) &&
              strncmp(response_token->valuestring, "vld_", 4) == 0;
    std::string device_token = ok ? response_token->valuestring : "";
    cJSON_Delete(response_root);

    if (!ok) {
        ESP_LOGE(TAG, "Voice Lab enrollment response did not contain a valid device credential");
        return false;
    }

    {
        Settings settings("voice_lab", true);
        settings.SetString("device_token", device_token);
        settings.EraseKey("enrollment_code");
    }
    ESP_LOGI(TAG, "Voice Lab device paired successfully; credential stored in NVS");
    return true;
}

bool VoiceLabClient::IsConnected() const { return connected_.load(); }

void VoiceLabClient::ConnectAsync() {
    if (!IsAutoConnectEnabled() || GetConfiguredHost().empty()) {
        return;
    }
    if (!EnsureControlTask())
        return;
    manual_disconnect_.store(false);
    EnsureHeartbeatTask();
    EnsureUsbProvisioningTask();

    bool expected = false;
    if (!connecting_.compare_exchange_strong(expected, true)) {
        return;
    }

    xTaskCreate(
        [](void* arg) {
            auto* client = static_cast<VoiceLabClient*>(arg);
            bool connected = client->Connect();
            if (!connected)
                client->SetUserState(UserState::Error);
            client->connecting_.store(false);
            if (!connected && !client->manual_disconnect_.load()) {
                client->ScheduleReconnect();
            }
            vTaskDelete(nullptr);
        },
        "voice_lab_conn", 4096 * 2, this, 2, nullptr);
}

bool VoiceLabClient::Connect() {
    if (!EnsureControlTask())
        return false;
    const auto generation = connection_generation_.load();
    if (GetConfiguredHost().empty()) {
        ESP_LOGI(TAG, "Voice Lab server host is empty, skip connect");
        return false;
    }
    if (!EnsureSystemTimeForTls(IsTlsEnabled())) {
        return false;
    }
    if (generation != connection_generation_.load() || manual_disconnect_.load())
        return false;
    if (!EnrollIfNeeded()) {
        return false;
    }
    if (generation != connection_generation_.load() || manual_disconnect_.load())
        return false;
    EnsureHeartbeatTask();

    auto device_key = GetDeviceKey();
    auto token = GetDeviceToken();
    auto control_url = BuildWebsocketUrl("/api/v1/devices/" + device_key + "/control");

    std::lock_guard<std::mutex> lock(mutex_);
    if (generation != connection_generation_.load() || manual_disconnect_.load())
        return false;
    if (control_websocket_ != nullptr && control_websocket_->IsConnected()) {
        connected_.store(true);
        return true;
    }
    if (control_websocket_ != nullptr) {
        control_websocket_.reset();
    }

    ESP_LOGI(TAG, "Connecting to Voice Lab control: %s", control_url.c_str());
    const bool result = ConnectControlLocked(control_url, token);
    if (generation != connection_generation_.load() || manual_disconnect_.load()) {
        connected_.store(false);
        if (control_websocket_)
            control_websocket_->Close();
        return false;
    }
    return result;
}

bool VoiceLabClient::ConnectControlLocked(const std::string& url, const std::string& token) {
    auto network = Board::GetInstance().GetNetwork();
    control_websocket_ = network->CreateWebSocket(3);
    // Assignment has disposed of the preceding socket and its callbacks.
    latest_idle_revision_.store(-1);
    active_revision_.store(-1);
    if (control_websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create Voice Lab control websocket");
        connected_.store(false);
        return false;
    }

    if (!token.empty()) {
        control_websocket_->SetHeader("X-Device-Token", token.c_str());
    }
    control_websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    control_websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    control_websocket_->SetHeader("Voice-Lab-Protocol", kControlSchemaVersion);
    control_websocket_->SetReceiveBufferSize(4096);

    control_websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            ESP_LOGW(TAG, "Ignoring unexpected binary control message, len=%u",
                     static_cast<unsigned>(len));
            return;
        }

        cJSON* root = cJSON_ParseWithLength(data, len);
        if (root == nullptr) {
            ESP_LOGW(TAG, "Invalid Voice Lab control JSON: %.*s", static_cast<int>(len), data);
            return;
        }
        HandleJson(root);
        cJSON_Delete(root);
    });

    control_websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Voice Lab control disconnected");
        connected_.store(false);
        control_epoch_.fetch_add(1);
        AbortRecording();
        if (!manual_disconnect_.load()) {
            ScheduleReconnect();
        }
    });

    if (!control_websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect Voice Lab control, code=%d",
                 control_websocket_->GetLastError());
        control_websocket_.reset();
        connected_.store(false);
        return false;
    }

    connected_.store(true);
    last_control_rx_us_.store(esp_timer_get_time());
    if (boot_id_ == 0) {
        // Keep stream sequence numbers monotonic for the entire boot. The
        // server deduplicates on (deviceKey, bootId), including separate tests.
        boot_id_ = (static_cast<uint64_t>(esp_random()) << 20) |
                   (static_cast<uint64_t>(esp_timer_get_time()) & 0xfffff);
    }
    SetUserState(UserState::Ready);
    SendHelloLocked();
    return true;
}

bool VoiceLabClient::ConnectAudioLocked(const std::string& url, const std::string& token) {
    if (audio_websocket_ != nullptr && audio_websocket_->IsConnected()) {
        audio_connected_.store(true);
        return true;
    }

    auto network = Board::GetInstance().GetNetwork();
    audio_websocket_ = network->CreateWebSocket(4);
    if (audio_websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create Voice Lab audio websocket");
        audio_connected_.store(false);
        return false;
    }

    if (!token.empty()) {
        audio_websocket_->SetHeader("X-Device-Token", token.c_str());
    }
    audio_websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    audio_websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    audio_websocket_->SetHeader("Voice-Lab-Audio-Protocol", kAudioProtocol);
    audio_websocket_->SetReceiveBufferSize(2048);

    audio_websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            ESP_LOGW(TAG, "Ignoring unexpected binary audio message, len=%u",
                     static_cast<unsigned>(len));
            return;
        }

        cJSON* root = cJSON_ParseWithLength(data, len);
        if (root == nullptr) {
            ESP_LOGW(TAG, "Invalid Voice Lab audio JSON: %.*s", static_cast<int>(len), data);
            return;
        }
        HandleAudioJson(root);
        cJSON_Delete(root);
    });

    audio_websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Voice Lab audio disconnected");
        audio_connected_.store(false);
        if (!audio_completed_.load())
            AbortRecording();
    });

    if (!audio_websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect Voice Lab audio, code=%d",
                 audio_websocket_->GetLastError());
        audio_websocket_.reset();
        audio_connected_.store(false);
        return false;
    }

    audio_frames_sent_ = 0;
    audio_bytes_sent_ = 0;
    last_audio_stats_us_ = esp_timer_get_time();
    audio_connected_.store(true);
    return true;
}

void VoiceLabClient::CloseAudioLocked() {
    if (audio_websocket_ != nullptr) {
        audio_websocket_->Close();
        audio_websocket_.reset();
    }
    audio_connected_.store(false);
}

void VoiceLabClient::Disconnect() {
    manual_disconnect_.store(true);
    connection_generation_.fetch_add(1);
    control_epoch_.fetch_add(1);
    AbortRecording();

    std::lock_guard<std::mutex> lock(mutex_);
    CloseAudioLocked();
    if (control_websocket_ != nullptr) {
        control_websocket_->Close();
        control_websocket_.reset();
    }
    connected_.store(false);
}

void VoiceLabClient::SetUserState(UserState state) {
    if (user_state_.exchange(state) == state)
        return;
    Application::GetInstance().Schedule([this]() {
        auto& board = Board::GetInstance();
        if (auto* led = board.GetLed())
            led->OnStateChanged();
#if CONFIG_VOICE_LAB_STANDALONE_MODE
        if (Application::GetInstance().GetDeviceState() == kDeviceStateWifiConfiguring)
            return;
        auto* display = board.GetDisplay();
        if (!display)
            return;
        switch (user_state_.load()) {
            case UserState::Ready:
                display->SetStatus("待机，请在网页开始测试");
                break;
            case UserState::Starting:
                display->SetStatus("正在连接录音");
                break;
            case UserState::Requesting:
                display->SetStatus("已请求开始，请等待授权");
                break;
            case UserState::Recording:
                display->SetStatus("正在录音");
                break;
            case UserState::Paused:
                display->SetStatus("录音已暂停，再次短按继续");
                break;
            case UserState::Pausing:
                display->SetStatus("正在确认暂停");
                break;
            case UserState::Stopping:
                display->SetStatus("已停止采集，正在整理");
                break;
            case UserState::Connecting:
                display->SetStatus("正在连接服务");
                break;
            case UserState::Error:
                display->SetStatus("录音已停止，请查看网页");
                break;
        }
#endif
    });
}

void VoiceLabClient::AbortRecording() {
    // No network calls or network mutex: this also runs from the safety timer.
    abort_generation_.fetch_add(1);
    recording_.store(false);
    capture_paused_.store(false);
    start_request_deadline_us_.store(0);
    interrupted_.store(true);
    Application::GetInstance().GetAudioService().EnableExternalCapture(false);
    SetUserState(UserState::Error);
}

void VoiceLabClient::NotifyNetworkDisconnected() {
    manual_disconnect_.store(true);
    connected_.store(false);
    connection_generation_.fetch_add(1);
    control_epoch_.fetch_add(1);
    AbortRecording();
}

bool VoiceLabClient::EnsureControlTask() {
    static const bool ready = [this]() {
        control_queue_ = xQueueCreate(8, sizeof(ControlMessage));
        if (!control_queue_)
            return false;
        if (xTaskCreate(
                [](void* arg) {
                    auto* client = static_cast<VoiceLabClient*>(arg);
                    while (true) {
                        ControlMessage message{};
                        if (xQueueReceive(client->control_queue_, &message, pdMS_TO_TICKS(100)) ==
                            pdTRUE) {
                            auto* type = cJSON_GetObjectItem(message.json, "type");
                            const bool toggle_pause =
                                JsonText(type, "local_toggle_recording_pause");
                            if (message.epoch == client->control_epoch_.load()) {
                                if (client->handled_control_epoch_ != message.epoch) {
                                    client->recording_guard_.Reset();
                                    client->handled_control_epoch_ = message.epoch;
                                }
                                if (toggle_pause) {
                                    auto* generation =
                                        cJSON_GetObjectItem(message.json, "captureGeneration");
                                    if (JsonInteger(generation, 0, UINT32_MAX))
                                        client->ToggleRecordingPause(
                                            message.epoch,
                                            static_cast<uint32_t>(generation->valuedouble));
                                } else if (strcmp(type->valuestring, "desired_config") == 0) {
                                    client->ApplyDesiredConfig(message.json, message.epoch,
                                                               message.received_at_us);
                                } else if (strcmp(type->valuestring, "recording_request_result") ==
                                               0 ||
                                           strcmp(type->valuestring, "recording_lease") == 0 ||
                                           strcmp(type->valuestring, "recording_stop") == 0) {
                                    client->HandleRecordingResponse(message.json);
                                } else if (strcmp(type->valuestring, "ping") == 0) {
                                    client->SendJson("{\"type\":\"pong\"}");
                                } else if (strcmp(type->valuestring, "playback") == 0) {
                                    auto* action = cJSON_GetObjectItem(message.json, "action");
                                    auto* url = cJSON_GetObjectItem(message.json, "url");
                                    if (cJSON_IsString(action) &&
                                        strcmp(action->valuestring, "play_url") == 0 &&
                                        cJSON_IsString(url)) {
                                        client->RequestPlayback(url->valuestring);
                                    } else if (cJSON_IsString(action) &&
                                               strcmp(action->valuestring, "stop") == 0) {
                                        client->StopPlayback();
                                    }
                                }
                            }
                            if (toggle_pause)
                                client->pause_toggle_pending_.store(false);
                            cJSON_Delete(message.json);
                        }
                        if (client->tail_pending_.load() && !client->recording_.load()) {
                            client->StopRecording();
                        } else if (client->recording_.load()) {
                            client->MaybeRenewRecordingLease();
                            std::lock_guard<std::mutex> lock(client->mutex_);
                            for (int i = 0; i < 2 && client->recording_.load(); ++i) {
                                if (!client->FlushPendingAudioLocked(false))
                                    break;
                            }
                            if (!client->RetransmitAudioLocked())
                                client->AbortRecording();
                        }
                    }
                },
                "voice_lab_control", 8192, this, 2, nullptr) != pdPASS) {
            vQueueDelete(control_queue_);
            control_queue_ = nullptr;
            return false;
        }
        esp_timer_create_args_t args{};
        args.callback = [](void* arg) {
            auto* client = static_cast<VoiceLabClient*>(arg);
            const auto now = esp_timer_get_time();
            auto requested_until = client->start_request_deadline_us_.load();
            if (requested_until > 0 && now >= requested_until &&
                client->start_request_deadline_us_.compare_exchange_strong(requested_until, 0)) {
                // Socket writes/control work can be blocked; customer feedback
                // must still time out independently of either worker.
                if (client->GetUserState() == UserState::Requesting)
                    client->SetUserState(UserState::Error);
                QueueVoiceLabPrompt(VoiceLabPrompt::StartFailed);
            }
            if (!client->recording_.load())
                return;
            const auto lease_deadline = client->lease_deadline_us_.load();
            if (VoiceLabRecordingLease::DeadlineExpired(client->recording_deadline_us_.load(),
                                                        now) ||
                VoiceLabRecordingLease::DeadlineExpired(lease_deadline, now) ||
                now - client->last_control_rx_us_.load() >= kControlSilenceLimitUs) {
                client->AbortRecording();
            }
        };
        args.arg = this;
        args.name = "voice_lab_safety";
        if (esp_timer_create(&args, &safety_timer_) != ESP_OK ||
            esp_timer_start_periodic(safety_timer_, 250000) != ESP_OK)
            return false;
        return true;
    }();
    return ready;
}

void VoiceLabClient::EnsureHeartbeatTask() {
    bool expected = false;
    if (!heartbeat_started_.compare_exchange_strong(expected, true)) {
        return;
    }

    xTaskCreate(
        [](void* arg) {
            auto* client = static_cast<VoiceLabClient*>(arg);
            while (true) {
                vTaskDelay(pdMS_TO_TICKS(kControlHeartbeatIntervalMs));
                if (!client->IsAutoConnectEnabled() || client->GetConfiguredHost().empty()) {
                    continue;
                }
                if (client->manual_disconnect_.load()) {
                    continue;
                }
                if (client->IsConnected()) {
                    // A spoken cue or capture-transition ACK can occupy the
                    // control worker. Lease requests must remain independent.
                    client->MaybeRenewRecordingLease();
                    if (!client->SendJson("{\"type\":\"ping\"}")) {
                        ESP_LOGW(TAG, "Voice Lab heartbeat ping failed");
                        client->connected_.store(false);
                        client->AbortRecording();
                        client->ScheduleReconnect();
                    }
                } else {
                    ESP_LOGI(TAG, "Voice Lab heartbeat detected disconnected control channel");
                    client->ScheduleReconnect();
                }
            }
        },
        "voice_lab_heartbeat", 4096, this, 2, nullptr);
}

void VoiceLabClient::EnsureUsbProvisioningTask() {
    bool expected = false;
    if (!g_usb_provisioning_task_started.compare_exchange_strong(expected, true)) {
        return;
    }

    xTaskCreate(
        [](void*) {
            ESP_LOGI(TAG, "Voice Lab USB provisioning ready. Type: vl pair XXXX-XXXX");
            char line[128];
            while (true) {
                if (fgets(line, sizeof(line), stdin) == nullptr) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }

                std::string command = TrimAsciiWhitespace(line);
                if (command.empty()) {
                    continue;
                }

                std::string code;
                if (command == "vl reset" || command == "voice_lab reset") {
                    VoiceLabClient::GetInstance().ResetVoiceLabSettings();
                    continue;
                }
                if (command == "vl unpair" || command == "voice_lab unpair") {
                    VoiceLabClient::GetInstance().ClearPairing();
                    continue;
                }
                if (command == "vl status" || command == "voice_lab status") {
                    ESP_LOGI(TAG, "Voice Lab status: %s",
                             VoiceLabClient::GetInstance().GetStatusJson().c_str());
                    continue;
                }

                if (ConsumePrefix(command, "vl pair ")) {
                    code = command;
                } else if (ConsumePrefix(command, "voice_lab pair ")) {
                    code = command;
                } else if (ConsumePrefix(command, "pair ")) {
                    code = command;
                } else {
                    auto normalized = NormalizeEnrollmentCode(command);
                    if (normalized.size() == 9) {
                        code = normalized;
                    }
                }

                if (code.empty()) {
                    continue;
                }

                ESP_LOGI(TAG, "Voice Lab pairing command received from USB");
                bool paired = VoiceLabClient::GetInstance().PairWithEnrollmentCode(code);
                if (paired) {
                    ESP_LOGI(TAG, "Voice Lab USB pairing succeeded; connecting control channel");
                    VoiceLabClient::GetInstance().ConnectAsync();
                } else {
                    ESP_LOGW(
                        TAG,
                        "Voice Lab USB pairing failed; generate a fresh enrollment code and retry");
                }
            }
        },
        "voice_lab_usb", 4096, nullptr, 2, nullptr);
}

void VoiceLabClient::ScheduleReconnect() {
    if (!IsAutoConnectEnabled() || GetConfiguredHost().empty() || manual_disconnect_.load()) {
        return;
    }

    bool expected = false;
    if (!reconnect_scheduled_.compare_exchange_strong(expected, true)) {
        return;
    }

    ESP_LOGI(TAG, "Voice Lab reconnect scheduled");
    xTaskCreate(
        [](void* arg) {
            auto* client = static_cast<VoiceLabClient*>(arg);
            vTaskDelay(pdMS_TO_TICKS(3000));
            client->reconnect_scheduled_.store(false);
            if (!client->manual_disconnect_.load()) {
                client->ConnectAsync();
            }
            vTaskDelete(nullptr);
        },
        "voice_lab_reconn", 4096, this, 2, nullptr);
}

void VoiceLabClient::SendHelloLocked() {
    auto app_desc = esp_app_get_description();
    auto device_key = GetDeviceKey();
    auto boot_id = std::to_string(boot_id_);
    std::string firmware_build = std::string(app_desc->date) + " " + app_desc->time;
    auto& wifi = WifiManager::GetInstance();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "controlSchemaVersion", kControlSchemaVersion);
    cJSON_AddNumberToObject(root, "recordingControlVersion", 1);
    cJSON_AddNumberToObject(root, "recordingFinalizationVersion", 1);
    cJSON_AddStringToObject(root, "deviceKey", device_key.c_str());
    cJSON_AddStringToObject(root, "deviceId", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(root, "clientId", Board::GetInstance().GetUuid().c_str());
    cJSON_AddStringToObject(root, "firmware", app_desc->version);
    cJSON_AddStringToObject(root, "firmwareVersion", app_desc->version);
    cJSON_AddStringToObject(root, "firmwareBuild", firmware_build.c_str());
    cJSON_AddStringToObject(root, "firmwareSha256", "unknown");
    cJSON_AddStringToObject(root, "sourceCommit", "unknown");
    cJSON_AddStringToObject(root, "hardwareProfile", GetHardwareProfile().c_str());
    cJSON_AddStringToObject(root, "boardName", BOARD_NAME);
    cJSON_AddStringToObject(root, "bootId", boot_id.c_str());
    cJSON_AddStringToObject(root, "capabilitySchemaVersion", kCapabilitySchemaVersion);
    cJSON_AddStringToObject(root, "audioFrontend", GetAudioFrontend().c_str());
    cJSON_AddStringToObject(root, "audioFrontendFirmware", kAudioFrontendFirmware);
    cJSON_AddNumberToObject(root, "rssi", wifi.GetRssi());
    cJSON_AddNumberToObject(root, "wifiChannel", wifi.GetChannel());
    cJSON_AddStringToObject(root, "localIp", wifi.GetIpAddress().c_str());
    cJSON_AddStringToObject(root, "serverHost", GetConfiguredHost().c_str());
    cJSON_AddNumberToObject(root, "serverPort", GetConfiguredPort());
    cJSON_AddBoolToObject(root, "serverTls", IsTlsEnabled());
    cJSON_AddStringToObject(root, "serverSource", "voice_lab_nvs_or_kconfig");
    cJSON_AddNumberToObject(root, "freeHeapBytes", esp_get_free_heap_size());
    cJSON_AddBoolToObject(root, "psramFound", heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0);
    cJSON_AddNumberToObject(root, "psramTotalBytes", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "freePsramBytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    cJSON* control_capabilities = cJSON_CreateArray();
    cJSON_AddItemToArray(control_capabilities, cJSON_CreateString("recording"));
    cJSON_AddItemToArray(control_capabilities, cJSON_CreateString("playback-url"));
    cJSON_AddItemToArray(control_capabilities, cJSON_CreateString(kAudioProtocol));
    cJSON_AddItemToObject(root, "controlCapabilities", control_capabilities);

    cJSON* capabilities = cJSON_CreateObject();
    cJSON_AddBoolToObject(capabilities, "recording", true);
    cJSON_AddBoolToObject(capabilities, "playbackUrl", true);
    cJSON_AddStringToObject(capabilities, "audioProtocol", kAudioProtocol);
    cJSON_AddNumberToObject(capabilities, "recordingControlVersion", 1);
    cJSON_AddNumberToObject(capabilities, "recordingFinalizationVersion", 1);
    cJSON_AddItemToObject(root, "capabilities", capabilities);

    cJSON* audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "encoding", "pcm_s16le");
    cJSON_AddNumberToObject(audio, "sampleRate", kAudioSampleRate);
    cJSON_AddNumberToObject(audio, "channels", kAudioChannels);
    cJSON_AddNumberToObject(audio, "frameDurationMs", kAudioFrameDurationMs);
    cJSON_AddStringToObject(audio, "inputSemantics",
                            "mono mixdown from Waveshare ESP32-S3 Audio Board ES7210 input");
    cJSON_AddItemToObject(root, "audioFormat", audio);

    control_websocket_->Send(PrintJson(root));
    cJSON_Delete(root);
}

std::string VoiceLabClient::PrintJson(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    std::string result = text != nullptr ? text : "{}";
    if (text != nullptr) {
        cJSON_free(text);
    }
    return result;
}

bool VoiceLabClient::SendJson(const std::string& json) {
    return SendJson(json, control_epoch_.load());
}

bool VoiceLabClient::SendJson(const std::string& json, uint32_t expected_epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (expected_epoch != control_epoch_.load() || control_websocket_ == nullptr ||
        !control_websocket_->IsConnected()) {
        return false;
    }
    return control_websocket_->Send(json);
}

bool VoiceLabClient::SendAudioJson(const std::string& json) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (audio_websocket_ == nullptr || !audio_websocket_->IsConnected()) {
        return false;
    }
    return audio_websocket_->Send(json);
}

bool VoiceLabClient::StartRecording(const std::string& recording_id, const std::string& mode) {
#if CONFIG_VOICE_LAB_STANDALONE_MODE
    // Local/MCP commands cannot grant customer capture authorization. Only a
    // fresh desired_config on the authenticated control channel starts capture.
    ESP_LOGW(TAG, "Start recording through the Voice Lab console");
    return false;
#else
    return StartAuthorizedRecording(recording_id, mode, -1, control_epoch_.load());
#endif
}

bool VoiceLabClient::RequestStartRecording() {
    if (!IsConnected() || recording_.load() || tail_pending_.load() ||
        GetUserState() == UserState::Starting) {
        QueueVoiceLabPrompt(VoiceLabPrompt::StartFailed);
        return false;
    }
    const auto deadline = esp_timer_get_time() + 10000000;
    int64_t expected = 0;
    if (!start_request_deadline_us_.compare_exchange_strong(expected, deadline)) {
        return true;  // Debounce across keys and repeated clicks.
    }
    const auto epoch = control_epoch_.load();
    const auto request_id = std::to_string(boot_id_) + "-" + std::to_string(esp_timer_get_time());
    {
        std::lock_guard<std::mutex> authorization(authorization_mutex_);
        pending_request_id_ = request_id;
    }
    SetUserState(UserState::Requesting);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "eventType", "recording_start_requested");
    cJSON_AddStringToObject(root, "requestId", request_id.c_str());
    cJSON_AddStringToObject(root, "mode", "meeting_live");
    cJSON_AddNumberToObject(root, "expiresAfterMs", 10000);
    cJSON_AddStringToObject(root, "bootId", std::to_string(boot_id_).c_str());
    const auto json = PrintJson(root);
    cJSON_Delete(root);
    bool sent = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (epoch != control_epoch_.load() || start_request_deadline_us_.load() != deadline ||
            esp_timer_get_time() >= deadline)
            return false;
        if (control_websocket_ && control_websocket_->IsConnected())
            sent = control_websocket_->Send(json);
    }
    // Do not resurrect a timed-out request or speak over a server-authorized
    // start that arrived while Send was blocked.
    if (start_request_deadline_us_.load() != deadline || esp_timer_get_time() >= deadline)
        return false;
    if (sent) {
        QueueVoiceLabPrompt(VoiceLabPrompt::StartRequested);
    } else {
        auto pending = deadline;
        if (start_request_deadline_us_.compare_exchange_strong(pending, 0)) {
            SetUserState(UserState::Error);
            QueueVoiceLabPrompt(VoiceLabPrompt::StartFailed);
        }
    }
    return sent;
}

bool VoiceLabClient::ApplyRecordingAuthorization(const cJSON* authorization,
                                                 int64_t received_at_us) {
    std::lock_guard<std::mutex> lock(authorization_mutex_);
    if (!authorization) {
        recording_lease_.Clear();
        active_session_id_.clear();
        lease_deadline_us_.store(0);
        recording_deadline_us_.store(0);
        session_stop_requested_.store(false);
        return true;
    }
    const auto* schema = cJSON_GetObjectItem(authorization, "schemaVersion");
    const auto* session = cJSON_GetObjectItem(authorization, "sessionId");
    const auto* request = cJSON_GetObjectItem(authorization, "requestId");
    const auto* source = cJSON_GetObjectItem(authorization, "source");
    const auto* boot = cJSON_GetObjectItem(authorization, "bootId");
    const auto* lease = cJSON_GetObjectItem(authorization, "leaseDurationMs");
    const auto* maximum = cJSON_GetObjectItem(authorization, "maxDurationMs");
    const bool from_device = JsonText(source, "device");
    if (!cJSON_IsObject(authorization) || !JsonText(schema, "voice-lab-recording-lease-v1") ||
        !cJSON_IsString(session) || !session->valuestring[0] ||
        strlen(session->valuestring) > 128 || (!from_device && !JsonText(source, "web")) ||
        !JsonText(boot, std::to_string(boot_id_).c_str()) || !JsonInteger(lease, 1000, 30000) ||
        !(JsonInteger(maximum, 0, 0) || JsonInteger(maximum, 60000, 3600000)))
        return false;
    const auto now = esp_timer_get_time();
    auto deadline = start_request_deadline_us_.load();
    if (from_device &&
        !VoiceLabRecordingLease::MatchesRequest(
            cJSON_IsString(request) && pending_request_id_ == request->valuestring, deadline, now))
        return false;
    // Validate before consuming the one pending local request. A timeout or
    // disconnect that wins this compare/exchange cannot be undone by a reply.
    if (!recording_lease_.Begin(received_at_us, now, lease->valueint, maximum->valueint))
        return false;
    if (from_device && !start_request_deadline_us_.compare_exchange_strong(deadline, 0)) {
        recording_lease_.Clear();
        return false;
    }
    active_session_id_ = session->valuestring;
    session_stop_requested_.store(false);
    lease_deadline_us_.store(recording_lease_.Deadline());
    recording_deadline_us_.store(recording_lease_.MaximumDeadline());
    return true;
}

bool VoiceLabClient::RequestToggleRecordingPause() {
    const auto generation = capture_generation_.load();
    const auto state = GetUserState();
    if (!IsConnected() || !recording_.load() ||
        (state != UserState::Recording && state != UserState::Paused) || !control_queue_)
        return false;
    bool expected = false;
    if (!pause_toggle_pending_.compare_exchange_strong(expected, true))
        return false;
    auto* root = cJSON_CreateObject();
    if (!root) {
        pause_toggle_pending_.store(false);
        return false;
    }
    cJSON_AddStringToObject(root, "type", "local_toggle_recording_pause");
    cJSON_AddNumberToObject(root, "captureGeneration", generation);
    ControlMessage message{root, control_epoch_.load(), esp_timer_get_time()};
    if (xQueueSend(control_queue_, &message, 0) != pdTRUE) {
        cJSON_Delete(root);
        pause_toggle_pending_.store(false);
        return false;
    }
    return true;
}

uint64_t VoiceLabClient::CaptureSampleEnd() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> pcm_lock(pcm_mutex_);
    return sample_start_ + pending_audio_pcm_.size() / kAudioChannels;
}

bool VoiceLabClient::ToggleRecordingPause(uint32_t authorized_epoch, uint32_t capture_generation) {
    std::lock_guard<std::mutex> operation(recording_mutex_);
    auto cancelled = [this, authorized_epoch, capture_generation]() {
        return !recording_.load() || interrupted_.load() || !IsConnected() ||
               authorized_epoch != control_epoch_.load() ||
               capture_generation != capture_generation_.load() || session_stop_requested_.load() ||
               VoiceLabRecordingGuard::StartSuperseded(recording_revision_,
                                                       latest_idle_revision_.load());
    };
    if (cancelled() || !audio_connected_.load())
        return false;
    auto& audio = Application::GetInstance().GetAudioService();
    if (!capture_paused_.load()) {
        if (!audio.StopExternalCaptureAndWait(1000)) {
            capture_stop_failed_.store(true);
            AbortRecording();
            return false;
        }
        if (cancelled())
            return false;
        capture_paused_.store(true);
        SetUserState(UserState::Pausing);
        const auto cutoff = CaptureSampleEnd();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (int i = 0; i < kMaxPendingPcmPackets; ++i) {
                if (!FlushPendingAudioLocked(true)) {
                    AbortRecording();
                    return false;
                }
            }
        }
        // Keep the stream open, but confirm the captured prefix before declaring
        // a pause on the independently delivered control channel.
        const auto deadline = esp_timer_get_time() + 2000000;
        while (!cancelled() && audio_connected_.load() &&
               acknowledged_sample_end_.load() < cutoff && esp_timer_get_time() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!RetransmitAudioLocked()) {
                    AbortRecording();
                    return false;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (cancelled())
            return false;
        if (!audio_connected_.load() || acknowledged_sample_end_.load() < cutoff) {
            AbortRecording();
            return false;
        }
        if (!SendCaptureTransition(true, cutoff))
            return false;
        if (cancelled())
            return false;
        SetUserState(UserState::Paused);
        SpeakVoiceLabPrompt(VoiceLabPrompt::RecordingPaused);
        return !cancelled();
    }

    // The microphone stays stopped during the resume announcement. Keep the
    // original session, PCM counters, authorization and lease renewal schedule.
    if (!SpeakVoiceLabPrompt(VoiceLabPrompt::RecordingResumed) || cancelled())
        return false;
    vTaskDelay(pdMS_TO_TICKS(150));
    if (cancelled() || !audio_connected_.load())
        return false;
    if (!SendCaptureTransition(false))
        return false;
    first_pcm_received_.store(false);
    {
        std::lock_guard<std::mutex> gate(capture_gate_mutex_);
        if (cancelled())
            return false;
        capture_paused_.store(false);
        audio.EnableExternalCapture(true);
    }
    SetUserState(UserState::Recording);
    const auto deadline = esp_timer_get_time() + 1000000;
    while (!first_pcm_received_.load() && !cancelled() && esp_timer_get_time() < deadline)
        vTaskDelay(pdMS_TO_TICKS(10));
    if (!first_pcm_received_.load()) {
        AbortRecording();
        return false;
    }
    return !cancelled();
}

bool VoiceLabClient::SendCaptureTransition(bool paused, uint64_t cutoff) {
    // Caller holds recording_mutex_, but receive callbacks use only the short
    // transition mutex so this bounded acknowledgement wait cannot block them.
    auto* status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "type", "status");
    cJSON_AddStringToObject(status, "mode", recording_mode_.c_str());
    cJSON_AddStringToObject(status, "captureState", paused ? "paused" : "recording");
    cJSON_AddBoolToObject(status, "paused", paused);
    cJSON_AddBoolToObject(status, "recording", !paused);
    if (paused)
        cJSON_AddNumberToObject(status, "captureSampleEnd", static_cast<double>(cutoff));
    AppendCaptureIdentity(status);
    uint64_t id;
    {
        std::lock_guard<std::mutex> lock(capture_transition_mutex_);
        id = ++capture_transition_sequence_;
        auto* session = cJSON_GetObjectItem(status, "sessionId");
        pending_capture_transition_ = {id,
                                       recording_control_epoch_,
                                       recording_id_,
                                       recording_revision_,
                                       std::to_string(boot_id_),
                                       cJSON_IsString(session) ? session->valuestring : "",
                                       paused};
    }
    cJSON_AddNumberToObject(status, "transitionId", static_cast<double>(id));
    const auto json = PrintJson(status);
    cJSON_Delete(status);
    const bool sent = SendJson(json, recording_control_epoch_);
    const auto deadline = esp_timer_get_time() + kCaptureTransitionAckTimeoutMs * 1000LL;
    bool success = false;
    while (sent && recording_.load() && !interrupted_.load() &&
           control_epoch_.load() == recording_control_epoch_ && !session_stop_requested_.load() &&
           !VoiceLabRecordingGuard::StartSuperseded(recording_revision_,
                                                    latest_idle_revision_.load()) &&
           esp_timer_get_time() < deadline) {
        {
            std::lock_guard<std::mutex> lock(capture_transition_mutex_);
            if (pending_capture_transition_.id == id && pending_capture_transition_.acknowledged) {
                success = pending_capture_transition_.success;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    {
        std::lock_guard<std::mutex> lock(capture_transition_mutex_);
        pending_capture_transition_ = {};
    }
    if (!success) {
        // A queued explicit stop will perform normal finalization itself.
        if (!session_stop_requested_.load() &&
            !VoiceLabRecordingGuard::StartSuperseded(recording_revision_,
                                                     latest_idle_revision_.load())) {
            ESP_LOGW(TAG, "Capture %s acknowledgement failed; stopping capture",
                     paused ? "pause" : "resume");
            AbortRecording();
        }
        return false;
    }
    ESP_LOGI(TAG, "Capture %s acknowledged: transition=%llu", paused ? "pause" : "resume",
             static_cast<unsigned long long>(id));
    return true;
}

void VoiceLabClient::HandleCaptureTransitionAck(const cJSON* root, uint32_t epoch) {
    std::lock_guard<std::mutex> lock(capture_transition_mutex_);
    auto& pending = pending_capture_transition_;
    const auto* id = cJSON_GetObjectItem(root, "transitionId");
    const auto* revision = cJSON_GetObjectItem(root, "recordingRevision");
    const auto* paused = cJSON_GetObjectItem(root, "paused");
    const auto* success = cJSON_GetObjectItem(root, "success");
    const auto* session = cJSON_GetObjectItem(root, "sessionId");
    const bool same_session = pending.session_id.empty()
                                  ? (!session || cJSON_IsNull(session) || JsonText(session, ""))
                                  : JsonText(session, pending.session_id.c_str());
    if (!pending.id || pending.acknowledged || pending.epoch != epoch ||
        epoch != control_epoch_.load() || !JsonInteger(id, 1, 9007199254740991LL) ||
        static_cast<uint64_t>(id->valuedouble) != pending.id ||
        !JsonInteger(revision, 0, INT_MAX) || revision->valueint != pending.revision ||
        !JsonText(cJSON_GetObjectItem(root, "recordingId"), pending.recording_id.c_str()) ||
        !JsonText(cJSON_GetObjectItem(root, "bootId"), pending.boot_id.c_str()) || !same_session ||
        !cJSON_IsBool(paused) || cJSON_IsTrue(paused) != pending.paused || !cJSON_IsBool(success))
        return;
    pending.success = cJSON_IsTrue(success);
    pending.acknowledged = true;
}

void VoiceLabClient::AppendRecordingIdentity(cJSON* root) const {
    std::lock_guard<std::mutex> authorization(authorization_mutex_);
    cJSON_AddStringToObject(root, "bootId", std::to_string(boot_id_).c_str());
    if (!active_session_id_.empty())
        cJSON_AddStringToObject(root, "sessionId", active_session_id_.c_str());
}

void VoiceLabClient::AppendCaptureIdentity(cJSON* root) const {
    AppendRecordingIdentity(root);
    if (!recording_id_.empty())
        cJSON_AddStringToObject(root, "recordingId", recording_id_.c_str());
    if (recording_revision_ >= 0)
        cJSON_AddNumberToObject(root, "recordingRevision", recording_revision_);
}

void VoiceLabClient::HandleRecordingResponse(const cJSON* root) {
    const auto* type = cJSON_GetObjectItem(root, "type");
    if (JsonText(type, "recording_request_result")) {
        const auto* request = cJSON_GetObjectItem(root, "requestId");
        const auto* accepted = cJSON_GetObjectItem(root, "accepted");
        if (!cJSON_IsString(request) || !cJSON_IsFalse(accepted))
            return;  // An acceptance receipt alone never authorizes capture.
        std::lock_guard<std::mutex> authorization(authorization_mutex_);
        auto deadline = start_request_deadline_us_.load();
        if (VoiceLabRecordingLease::MatchesRequest(pending_request_id_ == request->valuestring,
                                                   deadline, esp_timer_get_time()) &&
            start_request_deadline_us_.compare_exchange_strong(deadline, 0)) {
            SetUserState(UserState::Error);
            QueueVoiceLabPrompt(VoiceLabPrompt::StartFailed);
        }
        return;
    }
    bool stop = false;
    {
        std::lock_guard<std::mutex> authorization(authorization_mutex_);
        const auto* session = cJSON_GetObjectItem(root, "sessionId");
        const auto* boot = cJSON_GetObjectItem(root, "bootId");
        if (active_session_id_.empty() || !JsonText(session, active_session_id_.c_str()) ||
            !JsonText(boot, std::to_string(boot_id_).c_str()))
            return;
        if (JsonText(type, "recording_stop")) {
            stop = true;
        } else if (JsonText(type, "recording_lease") && recording_.load()) {
            const auto* id = cJSON_GetObjectItem(root, "renewalId");
            const auto* duration = cJSON_GetObjectItem(root, "leaseDurationMs");
            if (JsonInteger(id, 1, 9007199254740991LL) && JsonInteger(duration, 1000, 30000) &&
                recording_lease_.Renew(esp_timer_get_time(), static_cast<uint64_t>(id->valuedouble),
                                       duration->valueint))
                lease_deadline_us_.store(recording_lease_.Deadline());
        }
    }
    if (stop)
        StopRecording();
}

void VoiceLabClient::MaybeRenewRecordingLease() {
    const auto epoch = control_epoch_.load();
    cJSON* root = nullptr;
    {
        std::lock_guard<std::mutex> authorization(authorization_mutex_);
        if (!recording_.load() || active_session_id_.empty())
            return;
        const auto id = recording_lease_.RequestRenewal(esp_timer_get_time());
        if (!id)
            return;
        root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "event");
        cJSON_AddStringToObject(root, "eventType", "recording_lease_renew_requested");
        cJSON_AddStringToObject(root, "sessionId", active_session_id_.c_str());
        cJSON_AddStringToObject(root, "bootId", std::to_string(boot_id_).c_str());
        cJSON_AddNumberToObject(root, "renewalId", static_cast<double>(id));
    }
    const auto json = PrintJson(root);
    cJSON_Delete(root);
    SendJson(json, epoch);
}

bool VoiceLabClient::StartAuthorizedRecording(const std::string& recording_id,
                                              const std::string& mode, int revision,
                                              uint32_t authorized_epoch, const cJSON* authorization,
                                              int64_t received_at_us) {
    std::lock_guard<std::mutex> operation(recording_mutex_);
    if (authorized_epoch != control_epoch_.load() || !IsConnected() || recording_.load() ||
        tail_pending_.load())
        return false;
    if (!ApplyRecordingAuthorization(authorization, received_at_us))
        return false;
    // Keep the command's source connection even if waiting for a preceding
    // stop held recording_mutex_ across a disconnect/reconnect.
    const auto epoch = authorized_epoch;
    recording_control_epoch_ = epoch;
    recording_id_ = recording_id;
    recording_revision_ = revision;
    capture_generation_.fetch_add(1);
    capture_paused_.store(false);
    active_revision_.store(revision);
    SetUserState(UserState::Starting);
    auto& audio_service = Application::GetInstance().GetAudioService();
    if (!audio_service.StopExternalCaptureAndWait(1000)) {
        capture_stop_failed_.store(true);
        SetUserState(UserState::Error);
        return false;
    }
    capture_stop_failed_.store(false);
    start_request_deadline_us_.store(0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        CloseAudioLocked();
    }
    // Previous stream close callbacks have now finished. A new abort during
    // preparation must survive every following wait and state reset.
    const auto abort_generation = abort_generation_.load();
    auto cancelled = [this, epoch, revision, abort_generation]() {
        const auto lease_deadline = lease_deadline_us_.load();
        return interrupted_.load() || abort_generation != abort_generation_.load() ||
               epoch != control_epoch_.load() || !IsConnected() || session_stop_requested_.load() ||
               (lease_deadline > 0 && esp_timer_get_time() >= lease_deadline) ||
               VoiceLabRecordingGuard::StartSuperseded(revision, latest_idle_revision_.load());
    };
    interrupted_.store(false);
    SetUserState(UserState::Starting);
    if (cancelled() || !Application::GetInstance().PrepareVoiceLabCapture(6000) || cancelled()) {
        SetUserState(UserState::Error);
        return false;
    }

    auto device_key = GetDeviceKey();
    auto token = GetDeviceToken();
    auto audio_url = BuildWebsocketUrl("/api/v1/devices/" + device_key + "/audio");

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled()) {
            SetUserState(UserState::Error);
            return false;
        }
        audio_accepted_.store(false);
        audio_completed_.store(false);
        if (!ConnectAudioLocked(audio_url, token)) {
            SetUserState(UserState::Error);
            return false;
        }
        recording_mode_ = mode == "counter_file" ? "counter_file" : "meeting_live";
        {
            std::lock_guard<std::mutex> pcm_lock(pcm_mutex_);
            pending_audio_pcm_.clear();
            // Allocate before microphone capture, avoiding reallocations in
            // the producer when a socket write briefly falls behind.
            pending_audio_pcm_.reserve(kMaxPendingPcmPackets * kMaxFramesPerPacket *
                                       kPcmSamplesPerFrame);
        }
        unacknowledged_audio_.clear();
        acknowledged_sample_end_.store(sample_start_);
        sent_sample_end_.store(sample_start_);
        audio_frames_sent_ = 0;
        audio_bytes_sent_ = 0;
        last_audio_stats_us_ = 0;
    }

    const auto accept_deadline = esp_timer_get_time() + kAudioAcceptTimeoutMs * 1000LL;
    while (!audio_accepted_.load() && audio_connected_.load() && !cancelled() &&
           esp_timer_get_time() < accept_deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!audio_accepted_.load() || !audio_connected_.load() || cancelled()) {
        std::lock_guard<std::mutex> lock(mutex_);
        CloseAudioLocked();
        SetUserState(UserState::Error);
        return false;
    }
    audio_service.EnableVoiceProcessing(false);
    audio_service.EnableWakeWordDetection(false);
#if CONFIG_VOICE_LAB_STANDALONE_MODE
    if (!SpeakVoiceLabPrompt(VoiceLabPrompt::RecordingStarted)) {
        std::lock_guard<std::mutex> lock(mutex_);
        CloseAudioLocked();
        SetUserState(UserState::Error);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(150));  // Let the speaker tail settle before capture.
#endif
    if (!audio_connected_.load() || cancelled()) {
        std::lock_guard<std::mutex> lock(mutex_);
        CloseAudioLocked();
        SetUserState(UserState::Error);
        return false;
    }
    // A leased session's deadlines are anchored to command receipt. An absent
    // absolute deadline keeps administrator capture running until a stop or fault.
    audio_service.EnableVoiceProcessing(false);
    audio_service.EnableWakeWordDetection(false);
    first_pcm_received_.store(false);
    bool opened = false;
    {
        std::lock_guard<std::mutex> gate(capture_gate_mutex_);
        if (!cancelled()) {
            tail_pending_.store(true);
            recording_.store(true);
            audio_service.EnableExternalCapture(true);
            opened = true;
        }
    }
    if (!opened) {
        std::lock_guard<std::mutex> lock(mutex_);
        CloseAudioLocked();
        SetUserState(UserState::Error);
        return false;
    }
    // A disconnect can race with enabling the capture event bit.
    if (cancelled()) {
        AbortRecording();
        return false;
    }
    SetUserState(UserState::Recording);
    const auto capture_deadline = esp_timer_get_time() + 1000000;
    while (!first_pcm_received_.load() && recording_.load() && !cancelled() &&
           esp_timer_get_time() < capture_deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!first_pcm_received_.load() || !recording_.load() || cancelled()) {
        AbortRecording();
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "status");
    cJSON_AddStringToObject(root, "mode", recording_mode_.c_str());
    cJSON_AddBoolToObject(root, "recording", true);
    cJSON_AddBoolToObject(root, "paused", false);
    cJSON_AddStringToObject(root, "captureState", "recording");
    AppendCaptureIdentity(root);
    auto json = PrintJson(root);
    cJSON_Delete(root);
    SendJson(json, epoch);
    return recording_.load() && !cancelled();
}

bool VoiceLabClient::StopRecording() {
    std::lock_guard<std::mutex> operation(recording_mutex_);
    if (!tail_pending_.load())
        return !interrupted_.load();
    auto& audio_service = Application::GetInstance().GetAudioService();
    // Keep accepting the final in-flight capture callback until the input task
    // acknowledges the cutoff. Never hold the PCM/network mutex while waiting.
    const bool capture_stopped = audio_service.StopExternalCaptureAndWait(1000);
    capture_stop_failed_.store(!capture_stopped);
    recording_.store(false);
    capture_paused_.store(false);
    recording_deadline_us_.store(0);
    recording_mode_ = "idle";
    if (!capture_stopped)
        interrupted_.store(true);
    const uint64_t capture_sample_end = CaptureSampleEnd();
    SetUserState(UserState::Stopping);
    cJSON* stopped = cJSON_CreateObject();
    cJSON_AddStringToObject(stopped, "type", "status");
    cJSON_AddStringToObject(stopped, "mode", "idle");
    cJSON_AddBoolToObject(stopped, "recording", false);
    cJSON_AddBoolToObject(stopped, "paused", false);
    cJSON_AddStringToObject(stopped, "captureState", capture_stopped ? "stopped" : "stop_failed");
    if (capture_stopped)
        cJSON_AddNumberToObject(stopped, "captureSampleEnd",
                                static_cast<double>(capture_sample_end));
    cJSON_AddStringToObject(stopped, "finalizationState", "pending");
    AppendCaptureIdentity(stopped);
    const auto stopped_json = PrintJson(stopped);
    cJSON_Delete(stopped);
    SendJson(stopped_json, recording_control_epoch_);
    bool end_sent = false;
    const uint64_t cutoff = capture_sample_end;
    bool tail_sent = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (audio_websocket_ != nullptr && audio_websocket_->IsConnected()) {
            for (int i = 0; i < kMaxPendingPcmPackets; ++i) {
                if (!FlushPendingAudioLocked(true)) {
                    interrupted_.store(true);
                    break;
                }
            }
            tail_sent = true;
        }
    }
    // Give the cutoff packet a bounded retry opportunity before `end`. The
    // existing server drains and closes after `end`, so never send PCM after it.
    const auto ack_deadline = esp_timer_get_time() + 2000000;
    while (tail_sent && audio_connected_.load() && acknowledged_sample_end_.load() < cutoff &&
           esp_timer_get_time() < ack_deadline) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!RetransmitAudioLocked()) {
                interrupted_.store(true);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tail_sent && audio_websocket_ && audio_websocket_->IsConnected()) {
            end_sent = audio_websocket_->Send("{\"type\":\"end\"}");
        }
    }
    const auto deadline = esp_timer_get_time() + kAudioDrainTimeoutMs * 1000LL;
    while (end_sent && !audio_completed_.load() && audio_connected_.load() &&
           esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    const bool complete = capture_stopped && end_sent && audio_completed_.load() &&
                          acknowledged_sample_end_.load() >= cutoff && !interrupted_.load();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        CloseAudioLocked();
        {
            std::lock_guard<std::mutex> pcm_lock(pcm_mutex_);
            pending_audio_pcm_.clear();
        }
        unacknowledged_audio_.clear();
    }
    tail_pending_.store(false);
    interrupted_.store(!complete);
    SetUserState(complete ? UserState::Ready : UserState::Error);
    QueueVoiceLabPrompt(complete ? VoiceLabPrompt::RecordingStopped : VoiceLabPrompt::Interrupted);
    cJSON* final = cJSON_CreateObject();
    cJSON_AddStringToObject(final, "type", "status");
    cJSON_AddStringToObject(final, "mode", "idle");
    cJSON_AddBoolToObject(final, "recording", false);
    cJSON_AddBoolToObject(final, "paused", false);
    cJSON_AddStringToObject(final, "finalizationState",
                            complete ? "audio_confirmed" : "interrupted");
    if (!complete)
        cJSON_AddStringToObject(final, "error", "audio_not_fully_confirmed");
    AppendCaptureIdentity(final);
    const auto final_json = PrintJson(final);
    cJSON_Delete(final);
    SendJson(final_json, recording_control_epoch_);
    {
        std::lock_guard<std::mutex> authorization(authorization_mutex_);
        recording_lease_.Clear();
        active_session_id_.clear();
        lease_deadline_us_.store(0);
    }
    return complete;
}

bool VoiceLabClient::RequestPlayback(const std::string& url) {
#if CONFIG_VOICE_LAB_STANDALONE_MODE
    if (recording_.load() || tail_pending_.load() || GetUserState() == UserState::Starting)
        return false;
#endif
    if (!IsHttpUrl(url)) {
        ESP_LOGW(TAG, "Voice Lab playback URL must be http or https");
        return false;
    }

    bool started = Application::GetInstance().StartVoiceLabPlayback(url);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "name", started ? "playback_requested" : "playback_rejected");
    cJSON_AddStringToObject(root, "url", url.c_str());
    auto json = PrintJson(root);
    cJSON_Delete(root);
    SendJson(json);
    return started;
}

void VoiceLabClient::StopPlayback() {
    Application::GetInstance().StopVoiceLabPlayback();
    SendJson("{\"type\":\"event\",\"name\":\"playback_stopped\"}");
}

void VoiceLabClient::SetRecordingIndicator(bool recording) {
    Application::GetInstance().Schedule([]() {
        if (auto* led = Board::GetInstance().GetLed())
            led->OnStateChanged();
    });
}

void VoiceLabClient::NotifyRecordingStarted() {
    Application::GetInstance().Schedule(
        []() { Application::GetInstance().Alert("Voice Lab", "正在录音", "recording"); });
}

bool VoiceLabClient::SendPcmAudio(std::vector<int16_t>&& pcm) {
    if (!recording_.load() || capture_paused_.load()) {
        return false;
    }
    if (pcm.empty() || pcm.size() % kPcmSamplesPerFrame != 0) {
        ESP_LOGW(TAG, "Drop invalid PCM frame: samples=%u", static_cast<unsigned>(pcm.size()));
        return false;
    }

    std::lock_guard<std::mutex> lock(pcm_mutex_);
    if (!audio_connected_.load()) {
        audio_connected_.store(false);
        AbortRecording();
        return false;
    }

    if (!recording_.load() || capture_paused_.load()) {
        return false;
    }
    if (pending_audio_pcm_.size() + pcm.size() >
        static_cast<size_t>(kMaxPendingPcmPackets * kMaxFramesPerPacket * kPcmSamplesPerFrame)) {
        ESP_LOGW(TAG, "Voice Lab PCM buffer full; rejecting capture chunk");
        AbortRecording();
        return false;
    }
    pending_audio_pcm_.insert(pending_audio_pcm_.end(), pcm.begin(), pcm.end());
    first_pcm_received_.store(true);
    // The control worker transmits batches. Never perform socket I/O in the
    // microphone task, including when a server stops acknowledging audio.
    return true;
}

bool VoiceLabClient::FlushPendingAudioLocked(bool force) {
    std::unique_lock<std::mutex> pcm_lock(pcm_mutex_);
    if (pending_audio_pcm_.empty()) {
        return true;
    }
    const size_t pending_frames = pending_audio_pcm_.size() / kPcmSamplesPerFrame;
    const size_t frames_to_send =
        std::min(pending_frames, static_cast<size_t>(kMaxFramesPerPacket));
    if (frames_to_send == 0 ||
        (!force && frames_to_send < static_cast<size_t>(kMaxFramesPerPacket))) {
        return true;
    }
    if (audio_websocket_ == nullptr || !audio_websocket_->IsConnected()) {
        audio_connected_.store(false);
        return false;
    }
    while (!unacknowledged_audio_.empty() &&
           unacknowledged_audio_.front().sample_end <= acknowledged_sample_end_.load()) {
        unacknowledged_audio_.pop_front();
    }
    if (unacknowledged_audio_.size() >= kMaxUnacknowledgedPackets) {
        ESP_LOGW(TAG, "Voice Lab acknowledgement buffer full; stopping capture");
        AbortRecording();
        return false;
    }

    const size_t samples_to_send = frames_to_send * kPcmSamplesPerFrame;
    std::vector<int16_t> batch(pending_audio_pcm_.begin(),
                               pending_audio_pcm_.begin() + samples_to_send);
    pending_audio_pcm_.erase(pending_audio_pcm_.begin(),
                             pending_audio_pcm_.begin() + samples_to_send);
    const size_t remaining_frames = pending_audio_pcm_.size() / kPcmSamplesPerFrame;
    pcm_lock.unlock();
    auto packet = BuildPcmPacket(batch, audio_sequence_, sample_start_);
    unacknowledged_audio_.push_back(
        {packet, sample_start_ + samples_to_send, esp_timer_get_time(), 0});
    sent_sample_end_.store(sample_start_ + samples_to_send);
    // Reserve IDs even on a failed socket write: a partially successful write
    // must not make the next test reuse IDs the server may have already seen.
    audio_sequence_ += frames_to_send;
    sample_start_ += samples_to_send;
    if (audio_frames_sent_ == 0) {
        ESP_LOGI(TAG, "Voice Lab PCM first batch ready: frames=%u packet_bytes=%u",
                 static_cast<unsigned>(frames_to_send), static_cast<unsigned>(packet.size()));
    }
    bool sent = audio_websocket_->Send(packet.data(), packet.size(), true);
    if (!sent) {
        ESP_LOGW(TAG, "Failed to send Voice Lab PCM batch: frames=%u samples=%u",
                 static_cast<unsigned>(frames_to_send), static_cast<unsigned>(samples_to_send));
        AbortRecording();
        return false;
    }

    audio_frames_sent_ += frames_to_send;
    audio_bytes_sent_ += samples_to_send * sizeof(int16_t);
    auto now = static_cast<uint64_t>(esp_timer_get_time());
    if (now - last_audio_stats_us_ >= 1000000) {
        ESP_LOGI(TAG,
                 "Voice Lab PCM sent: frames=%llu bytes=%llu sample_start=%llu pending_frames=%u",
                 static_cast<unsigned long long>(audio_frames_sent_),
                 static_cast<unsigned long long>(audio_bytes_sent_),
                 static_cast<unsigned long long>(sample_start_),
                 static_cast<unsigned>(remaining_frames));
        last_audio_stats_us_ = now;
    }
    return true;
}

bool VoiceLabClient::RetransmitAudioLocked() {
    while (!unacknowledged_audio_.empty() &&
           unacknowledged_audio_.front().sample_end <= acknowledged_sample_end_.load()) {
        unacknowledged_audio_.pop_front();
    }
    if (unacknowledged_audio_.empty())
        return true;
    if (!audio_websocket_ || !audio_websocket_->IsConnected())
        return false;
    const auto now = esp_timer_get_time();
    // Retain original packet IDs so a retry cannot duplicate captured speech.
    // Retry in sequence order, at most twice; never grow the buffer indefinitely.
    for (auto& packet : unacknowledged_audio_) {
        if (now - packet.last_sent_us < 1500000)
            break;
        if (packet.retries >= 2)
            return false;
        if (!audio_websocket_->Send(packet.bytes.data(), packet.bytes.size(), true))
            return false;
        packet.last_sent_us = now;
        ++packet.retries;
    }
    return true;
}

std::string VoiceLabClient::BuildPcmPacket(const std::vector<int16_t>& pcm, uint64_t first_sequence,
                                           uint64_t first_sample_start) const {
    const uint32_t samples_per_channel = static_cast<uint32_t>(pcm.size());
    const uint8_t frame_count = static_cast<uint8_t>(samples_per_channel / kPcmSamplesPerFrame);

    std::string packet;
    packet.reserve(kPcmPacketHeaderSize + pcm.size() * sizeof(int16_t));
    packet.append("VLA2", 4);
    packet.push_back(2);
    packet.push_back(0);
    packet.push_back(static_cast<char>(kAudioChannels));
    packet.push_back(static_cast<char>(frame_count));
    AppendLe64(packet, boot_id_);
    AppendLe64(packet, first_sequence);
    AppendLe64(packet, first_sample_start);
    AppendLe32(packet, static_cast<uint32_t>(esp_timer_get_time() / 1000));
    AppendLe32(packet, samples_per_channel);

    for (int16_t sample : pcm) {
        uint16_t value = static_cast<uint16_t>(sample);
        packet.push_back(static_cast<char>(value & 0xff));
        packet.push_back(static_cast<char>((value >> 8) & 0xff));
    }

    return packet;
}

void VoiceLabClient::SendConfigAck(uint32_t authorized_epoch, int revision, const std::string& mode,
                                   const cJSON* capture_plan, bool success,
                                   const std::string& error) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "config_ack");
    cJSON_AddNumberToObject(root, "revision", revision);
    cJSON_AddBoolToObject(root, "success", success);
    cJSON_AddStringToObject(root, "mode", mode.c_str());
    if (capture_plan != nullptr) {
        cJSON* copy = cJSON_Duplicate(capture_plan, true);
        if (copy != nullptr) {
            cJSON_AddItemToObject(root, "appliedCapturePlan", copy);
        }
    }
    if (!error.empty()) {
        cJSON_AddStringToObject(root, "error", error.c_str());
    }

    auto json = PrintJson(root);
    cJSON_Delete(root);
    SendJson(json, authorized_epoch);
}

void VoiceLabClient::ApplyDesiredConfig(const cJSON* root, uint32_t authorized_epoch,
                                        int64_t received_at_us) {
    if (authorized_epoch != control_epoch_.load())
        return;
    auto revision_item = cJSON_GetObjectItem(root, "revision");
    auto mode_item = cJSON_GetObjectItem(root, "mode");
    auto capture_plan = cJSON_GetObjectItem(root, "capturePlan");

    if (!cJSON_IsNumber(revision_item) || !std::isfinite(revision_item->valuedouble) ||
        revision_item->valuedouble < 0 || revision_item->valuedouble > INT_MAX ||
        revision_item->valuedouble != std::floor(revision_item->valuedouble)) {
        SendConfigAck(authorized_epoch, 0, "idle", nullptr, false, "invalid_revision");
        return;
    }
    int revision = revision_item->valueint;
    std::string mode = cJSON_IsString(mode_item) ? mode_item->valuestring : "";
    const bool active = mode == "meeting_live" || mode == "counter_file" || mode == "recording" ||
                        mode == "capture";
    const bool idle = mode == "idle" || mode == "standby" || mode == "off";
    if (!active && !idle) {
        SendConfigAck(authorized_epoch, revision, mode, capture_plan, false, "unsupported_mode");
        return;
    }
    const auto decision = recording_guard_.Apply(revision, active, recording_.load());
    using Decision = VoiceLabRecordingGuard::Decision;
    const std::string applied_mode =
        active ? (mode == "counter_file" ? "counter_file" : "meeting_live") : "idle";
    if (decision == Decision::NeedIdle || decision == Decision::Stale ||
        decision == Decision::Busy) {
        const char* error = decision == Decision::NeedIdle ? "idle_required_after_connect"
                            : decision == Decision::Busy   ? "recording_already_active"
                                                           : "stale_revision";
        SendConfigAck(authorized_epoch, revision, "idle", nullptr, false, error);
    } else if (decision == Decision::Duplicate) {
        bool compatible = true;
        if (active) {
            std::lock_guard<std::mutex> operation(recording_mutex_);
            compatible = recording_.load() && recording_mode_ == applied_mode;
            const auto* authorization = cJSON_GetObjectItem(root, "recordingAuthorization");
            std::lock_guard<std::mutex> lease_lock(authorization_mutex_);
            // Reusing a revision for a different customer session must never
            // acknowledge that new session as if it had started.
            if (authorization) {
                compatible = compatible && !active_session_id_.empty() &&
                             JsonText(cJSON_GetObjectItem(authorization, "sessionId"),
                                      active_session_id_.c_str()) &&
                             JsonText(cJSON_GetObjectItem(authorization, "bootId"),
                                      std::to_string(boot_id_).c_str());
            } else {
                compatible = compatible && active_session_id_.empty();
            }
        }
        SendConfigAck(authorized_epoch, revision, applied_mode, capture_plan, compatible,
                      compatible ? "" : "revision_conflict");
    } else if (decision == Decision::Start) {
        auto* test = cJSON_GetObjectItem(root, "test");
        auto* id = cJSON_GetObjectItem(test, "correlationId");
        std::string recording_id = cJSON_IsString(id) ? id->valuestring : "";
        if (recording_id.size() > 128) {
            SendConfigAck(authorized_epoch, revision, "idle", nullptr, false,
                          "invalid_recording_id");
            return;
        }
        bool started = StartAuthorizedRecording(
            recording_id, applied_mode, revision, authorized_epoch,
            cJSON_GetObjectItem(root, "recordingAuthorization"), received_at_us);
        if (authorized_epoch != control_epoch_.load())
            return;  // Never acknowledge an old command on a new connection.
        SendConfigAck(authorized_epoch, revision, started ? applied_mode : "idle", capture_plan,
                      started, started ? "" : "failed_to_start_recording");
    } else {
        const bool finalized = StopRecording();
        // config_ack confirms capture is stopped; finalizationState separately
        // reports whether the server confirmed the complete audio cutoff.
        SendConfigAck(authorized_epoch, revision, "idle", capture_plan,
                      !recording_.load() && !capture_stop_failed_.load(),
                      capture_stop_failed_.load() ? "capture_stop_timeout" : "");
        if (finalized && !tail_pending_.load() && !capture_stop_failed_.load())
            SetUserState(UserState::Ready);
    }
}

void VoiceLabClient::HandleJson(const cJSON* root) {
    auto type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        return;
    }
    last_control_rx_us_.store(esp_timer_get_time());
    if (strcmp(type->valuestring, "recording_pause_ack") == 0) {
        HandleCaptureTransitionAck(root, control_epoch_.load());
        return;
    }
    if (strcmp(type->valuestring, "recording_lease") == 0) {
        HandleRecordingResponse(root);
        return;
    }
    if (strcmp(type->valuestring, "desired_config") != 0 &&
        strcmp(type->valuestring, "ping") != 0 && strcmp(type->valuestring, "playback") != 0 &&
        strcmp(type->valuestring, "recording_request_result") != 0 &&
        strcmp(type->valuestring, "recording_lease") != 0 &&
        strcmp(type->valuestring, "recording_stop") != 0)
        return;
    if (JsonText(type, "recording_stop")) {
        std::lock_guard<std::mutex> authorization(authorization_mutex_);
        if (!active_session_id_.empty() &&
            JsonText(cJSON_GetObjectItem(root, "sessionId"), active_session_id_.c_str()) &&
            JsonText(cJSON_GetObjectItem(root, "bootId"), std::to_string(boot_id_).c_str())) {
            // Close capture immediately, even during the spoken start prompt;
            // the worker still performs the normal cutoff and tail handshake.
            std::lock_guard<std::mutex> gate(capture_gate_mutex_);
            session_stop_requested_.store(true);
            abort_generation_.fetch_add(1);
            Application::GetInstance().GetAudioService().EnableExternalCapture(false);
            if (recording_.load() || GetUserState() == UserState::Starting)
                SetUserState(UserState::Stopping);
        }
    }
    if (strcmp(type->valuestring, "desired_config") == 0) {
        const auto* revision = cJSON_GetObjectItem(root, "revision");
        const auto* mode = cJSON_GetObjectItem(root, "mode");
        const bool idle = cJSON_IsString(mode) && (strcmp(mode->valuestring, "idle") == 0 ||
                                                   strcmp(mode->valuestring, "standby") == 0 ||
                                                   strcmp(mode->valuestring, "off") == 0);
        if (idle && cJSON_IsNumber(revision) && std::isfinite(revision->valuedouble) &&
            revision->valuedouble >= 0 && revision->valuedouble <= INT_MAX &&
            revision->valuedouble == std::floor(revision->valuedouble)) {
            // An idle must close the gate even while the serialized worker is
            // waiting for the start announcement or a network write. Stale
            // idle revisions cannot cancel a newer authorized recording.
            std::lock_guard<std::mutex> gate(capture_gate_mutex_);
            if (revision->valueint > latest_idle_revision_.load())
                latest_idle_revision_.store(revision->valueint);
            if (revision->valueint > active_revision_.load()) {
                Application::GetInstance().GetAudioService().EnableExternalCapture(false);
                if (GetUserState() == UserState::Starting)
                    SetUserState(UserState::Stopping);
            }
        }
    }
    ControlMessage message{cJSON_Duplicate(root, true), control_epoch_.load(),
                           esp_timer_get_time()};
    if (!message.json || !control_queue_ || xQueueSend(control_queue_, &message, 0) != pdTRUE) {
        cJSON_Delete(message.json);
        ESP_LOGW(TAG, "Voice Lab control queue full; stopping capture");
        AbortRecording();
    }
}

void VoiceLabClient::HandleAudioJson(const cJSON* root) {
    auto type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "ingress_ack") == 0) {
        auto sequence = cJSON_GetObjectItem(root, "sequence");
        auto sample_end = cJSON_GetObjectItem(root, "sampleEnd");
        auto boot = cJSON_GetObjectItem(root, "bootId");
        bool matches_boot =
            (cJSON_IsString(boot) && std::to_string(boot_id_) == boot->valuestring) ||
            (cJSON_IsNumber(boot) && boot->valuedouble == static_cast<double>(boot_id_));
        if (!matches_boot || !cJSON_IsNumber(sample_end) ||
            !std::isfinite(sample_end->valuedouble) || sample_end->valuedouble < 0 ||
            sample_end->valuedouble > static_cast<double>(sent_sample_end_.load()) ||
            sample_end->valuedouble != std::floor(sample_end->valuedouble))
            return;
        const auto end = static_cast<uint64_t>(sample_end->valuedouble);
        auto previous = acknowledged_sample_end_.load();
        while (end > previous && !acknowledged_sample_end_.compare_exchange_weak(previous, end)) {
        }
        auto dropped = cJSON_GetObjectItem(root, "serverQueueDroppedFrames");
        if (cJSON_IsNumber(dropped) && dropped->valuedouble > 0) {
            ESP_LOGW(TAG, "Server dropped audio; marking this test interrupted");
            AbortRecording();
        }
        ESP_LOGI(TAG, "Voice Lab ingress ack: sequence=%lld sample_end=%lld",
                 cJSON_IsNumber(sequence) ? static_cast<long long>(sequence->valuedouble) : -1,
                 cJSON_IsNumber(sample_end) ? static_cast<long long>(sample_end->valuedouble) : -1);
        return;
    }

    auto status = cJSON_GetObjectItem(root, "status");
    if (cJSON_IsString(status)) {
        ESP_LOGI(TAG, "Voice Lab audio status: %s", status->valuestring);
        if (strcmp(status->valuestring, "accepted") == 0)
            audio_accepted_.store(true);
        else if (strcmp(status->valuestring, "completed") == 0) {
            audio_completed_.store(true);
            if (recording_.load())
                AbortRecording();
        } else
            AbortRecording();
    }
}

std::string VoiceLabClient::GetStatusJson() const {
    auto device_key = GetDeviceKey();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", IsConnected());
    cJSON_AddBoolToObject(root, "audio_connected", audio_connected_.load());
    cJSON_AddBoolToObject(root, "recording", recording_.load() && !capture_paused_.load());
    cJSON_AddBoolToObject(root, "recording_session_active", recording_.load());
    cJSON_AddBoolToObject(root, "paused", capture_paused_.load());
    cJSON_AddBoolToObject(root, "audio_finalization_pending",
                          tail_pending_.load() && !recording_.load());
    cJSON_AddBoolToObject(root, "interrupted", interrupted_.load());
    cJSON_AddNumberToObject(root, "legacy_recording_limit_seconds", 0);
    cJSON_AddStringToObject(root, "recording_authorization",
                            "session_lease_or_fresh_admin_revision_after_idle");
    cJSON_AddNumberToObject(root, "recording_control_version", 1);
    cJSON_AddBoolToObject(root, "session_lease_active", lease_deadline_us_.load() > 0);
    AppendRecordingIdentity(root);
    cJSON_AddBoolToObject(root, "auto_connect", IsAutoConnectEnabled());
    cJSON_AddStringToObject(root, "server_host", GetConfiguredHost().c_str());
    cJSON_AddNumberToObject(root, "server_port", GetConfiguredPort());
    cJSON_AddBoolToObject(root, "server_tls", IsTlsEnabled());
    cJSON_AddStringToObject(root, "server_url", GetConfiguredServerUrl().c_str());
    cJSON_AddStringToObject(root, "device_key", device_key.c_str());
    cJSON_AddStringToObject(root, "hardware_profile", GetHardwareProfile().c_str());
    cJSON_AddStringToObject(root, "audio_frontend", GetAudioFrontend().c_str());
    cJSON_AddBoolToObject(root, "paired", !GetDeviceToken().empty());
    cJSON_AddBoolToObject(root, "has_enrollment_code", !GetEnrollmentCode().empty());
    cJSON_AddStringToObject(root, "audio_protocol", kAudioProtocol);
    cJSON_AddNumberToObject(root, "recording_sample_rate", kAudioSampleRate);
    cJSON_AddNumberToObject(root, "recording_channels", kAudioChannels);
    cJSON_AddNumberToObject(root, "recording_frame_duration", kAudioFrameDurationMs);
    cJSON_AddBoolToObject(root, "playback_url", true);

    auto result = PrintJson(root);
    cJSON_Delete(root);
    return result;
}

void VoiceLabClient::RegisterMcpTools() {
    EnsureControlTask();
    EnsureUsbProvisioningTask();
    SetRecordingIndicator(false);

    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddUserOnlyTool(
        "self.voice_lab.configure",
        "Configure ESP-side Voice Lab host, port, TLS, device key, enrollment code, and "
        "auto-connect behavior.",
        PropertyList({
            Property("server_host", kPropertyTypeString, std::string(CONFIG_VOICE_LAB_SERVER_HOST)),
            Property("server_port", kPropertyTypeInteger, CONFIG_VOICE_LAB_SERVER_PORT, 1, 65535),
            Property("server_tls", kPropertyTypeBoolean,
                     static_cast<bool>(CONFIG_VOICE_LAB_SERVER_TLS)),
            Property("device_key", kPropertyTypeString, std::string(CONFIG_VOICE_LAB_DEVICE_KEY)),
            Property("hardware_profile", kPropertyTypeString,
                     std::string(CONFIG_VOICE_LAB_HARDWARE_PROFILE)),
            Property("audio_frontend", kPropertyTypeString,
                     std::string(CONFIG_VOICE_LAB_AUDIO_FRONTEND)),
            Property("enrollment_code", kPropertyTypeString, std::string("")),
            Property("auto_connect", kPropertyTypeBoolean, true),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string previous_device_key;
            {
                Settings current("voice_lab", false);
                previous_device_key = current.GetString("device_key", CONFIG_VOICE_LAB_DEVICE_KEY);
            }
            auto next_device_key = properties["device_key"].value<std::string>();
            auto enrollment_code =
                NormalizeEnrollmentCode(properties["enrollment_code"].value<std::string>());

            Settings settings("voice_lab", true);
            settings.EraseKey("server_url");
            settings.SetString("server_host", properties["server_host"].value<std::string>());
            settings.SetInt("server_port", properties["server_port"].value<int>());
            settings.SetBool("server_tls", properties["server_tls"].value<bool>());
            settings.SetString("device_key", next_device_key);
            settings.SetString("hw_profile", properties["hardware_profile"].value<std::string>());
            settings.SetString("audio_frontend", properties["audio_frontend"].value<std::string>());
            if (!enrollment_code.empty()) {
                settings.SetString("enrollment_code", enrollment_code);
            }
            if (next_device_key != previous_device_key || !enrollment_code.empty()) {
                settings.EraseKey("device_token");
                settings.EraseKey("token");
            }
            settings.SetBool("auto_connect", properties["auto_connect"].value<bool>());
            return true;
        });

    mcp_server.AddUserOnlyTool(
        "self.voice_lab.pair",
        "Exchange a short Voice Lab enrollment code for a device credential and store it in NVS.",
        PropertyList({
            Property("enrollment_code", kPropertyTypeString),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            return VoiceLabClient::GetInstance().PairWithEnrollmentCode(
                properties["enrollment_code"].value<std::string>());
        });

    mcp_server.AddUserOnlyTool(
        "self.voice_lab.unpair",
        "Clear the stored Voice Lab device credential and enrollment code from NVS.",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            VoiceLabClient::GetInstance().ClearPairing();
            return true;
        });

    mcp_server.AddUserOnlyTool(
        "self.voice_lab.reset",
        "Reset only Voice Lab NVS settings; Wi-Fi credentials are not changed.", PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            VoiceLabClient::GetInstance().ResetVoiceLabSettings();
            return true;
        });

    mcp_server.AddUserOnlyTool(
        "self.voice_lab.get_status",
        "Get Voice Lab connection, recording, playback and audio capability status.",
        PropertyList(), [](const PropertyList&) -> ReturnValue {
            return VoiceLabClient::GetInstance().GetStatusJson();
        });

    mcp_server.AddUserOnlyTool(
        "self.voice_lab.connect", "Connect to Voice Lab immediately.", PropertyList(),
        [](const PropertyList&) -> ReturnValue { return VoiceLabClient::GetInstance().Connect(); });

    mcp_server.AddUserOnlyTool("self.voice_lab.disconnect",
                               "Disconnect from Voice Lab and stop recording.", PropertyList(),
                               [](const PropertyList&) -> ReturnValue {
                                   VoiceLabClient::GetInstance().Disconnect();
                                   return true;
                               });

    mcp_server.AddTool("self.voice_lab.start_recording",
                       "Start streaming microphone audio to Voice Lab as 16 kHz mono PCM frames.",
                       PropertyList({
                           Property("recording_id", kPropertyTypeString, std::string("")),
                       }),
                       [](const PropertyList& properties) -> ReturnValue {
                           return VoiceLabClient::GetInstance().StartRecording(
                               properties["recording_id"].value<std::string>());
                       });

    mcp_server.AddTool("self.voice_lab.stop_recording",
                       "Stop streaming microphone audio to Voice Lab.", PropertyList(),
                       [](const PropertyList&) -> ReturnValue {
                           return VoiceLabClient::GetInstance().StopRecording();
                       });

    mcp_server.AddTool("self.voice_lab.play_url",
                       "Play an HTTP(S) Ogg audio URL on the device through the ESP playback path.",
                       PropertyList({
                           Property("url", kPropertyTypeString),
                       }),
                       [](const PropertyList& properties) -> ReturnValue {
                           return VoiceLabClient::GetInstance().RequestPlayback(
                               properties["url"].value<std::string>());
                       });

    mcp_server.AddTool("self.voice_lab.stop_playback",
                       "Stop current Voice Lab playback on the device.", PropertyList(),
                       [](const PropertyList&) -> ReturnValue {
                           VoiceLabClient::GetInstance().StopPlayback();
                           return true;
                       });
}
