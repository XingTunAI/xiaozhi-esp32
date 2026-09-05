#include "voice_lab_client.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "led/circular_strip.h"
#include "mcp_server.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <http.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

#include <algorithm>
#include <cctype>
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
std::atomic<bool> g_usb_provisioning_task_started{false};
std::atomic<bool> g_sntp_started{false};

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

VoiceLabClient::~VoiceLabClient() {
    Disconnect();
}

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
    return std::string(IsTlsEnabled() ? "https://" : "http://") + GetConfiguredHost() +
           ":" + std::to_string(GetConfiguredPort()) + path;
}

std::string VoiceLabClient::BuildWebsocketUrl(const std::string& path) const {
    return std::string(IsTlsEnabled() ? "wss://" : "ws://") + GetConfiguredHost() +
           ":" + std::to_string(GetConfiguredPort()) + path;
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
        ESP_LOGE(TAG, "Invalid Voice Lab config: NoTls is set on a TLS port. Use voice-lab.cloud:1883 tls=true or 106.55.21.5:18100 tls=false");
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
              device_key == response_device_key->valuestring &&
              cJSON_IsString(response_token) &&
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

bool VoiceLabClient::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return control_websocket_ != nullptr && control_websocket_->IsConnected() && connected_.load();
}

void VoiceLabClient::ConnectAsync() {
    if (!IsAutoConnectEnabled() || GetConfiguredHost().empty()) {
        return;
    }
    manual_disconnect_.store(false);
    EnsureHeartbeatTask();
    EnsureUsbProvisioningTask();

    bool expected = false;
    if (!connecting_.compare_exchange_strong(expected, true)) {
        return;
    }

    xTaskCreate([](void* arg) {
        auto* client = static_cast<VoiceLabClient*>(arg);
        bool connected = client->Connect();
        client->connecting_.store(false);
        if (!connected && !client->manual_disconnect_.load()) {
            client->ScheduleReconnect();
        }
        vTaskDelete(nullptr);
    }, "voice_lab_conn", 4096 * 2, this, 2, nullptr);
}

bool VoiceLabClient::Connect() {
    if (GetConfiguredHost().empty()) {
        ESP_LOGI(TAG, "Voice Lab server host is empty, skip connect");
        return false;
    }
    if (!EnsureSystemTimeForTls(IsTlsEnabled())) {
        return false;
    }
    if (!EnrollIfNeeded()) {
        return false;
    }
    manual_disconnect_.store(false);
    EnsureHeartbeatTask();

    auto device_key = GetDeviceKey();
    auto token = GetDeviceToken();
    auto control_url = BuildWebsocketUrl("/api/v1/devices/" + device_key + "/control");

    std::lock_guard<std::mutex> lock(mutex_);
    if (control_websocket_ != nullptr && control_websocket_->IsConnected()) {
        connected_.store(true);
        return true;
    }
    if (control_websocket_ != nullptr) {
        control_websocket_.reset();
    }

    ESP_LOGI(TAG, "Connecting to Voice Lab control: %s", control_url.c_str());
    return ConnectControlLocked(control_url, token);
}

bool VoiceLabClient::ConnectControlLocked(const std::string& url, const std::string& token) {
    auto network = Board::GetInstance().GetNetwork();
    control_websocket_ = network->CreateWebSocket(3);
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
            ESP_LOGW(TAG, "Invalid Voice Lab control JSON: %.*s",
                     static_cast<int>(len), data);
            return;
        }
        HandleJson(root);
        cJSON_Delete(root);
    });

    control_websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Voice Lab control disconnected");
        connected_.store(false);
        recording_.store(false);
        audio_connected_.store(false);
        Application::GetInstance().GetAudioService().EnableExternalCapture(false);
        SetRecordingIndicator(false);
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
    boot_id_ = static_cast<uint64_t>(esp_timer_get_time());
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
            ESP_LOGW(TAG, "Invalid Voice Lab audio JSON: %.*s",
                     static_cast<int>(len), data);
            return;
        }
        HandleAudioJson(root);
        cJSON_Delete(root);
    });

    audio_websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Voice Lab audio disconnected");
        audio_connected_.store(false);
        recording_.store(false);
        Application::GetInstance().GetAudioService().EnableExternalCapture(false);
        SetRecordingIndicator(false);
    });

    if (!audio_websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect Voice Lab audio, code=%d",
                 audio_websocket_->GetLastError());
        audio_websocket_.reset();
        audio_connected_.store(false);
        return false;
    }

    audio_sequence_ = 0;
    sample_start_ = 0;
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
    StopRecording();

    std::lock_guard<std::mutex> lock(mutex_);
    CloseAudioLocked();
    if (control_websocket_ != nullptr) {
        control_websocket_->Close();
        control_websocket_.reset();
    }
    connected_.store(false);
}

void VoiceLabClient::EnsureHeartbeatTask() {
    bool expected = false;
    if (!heartbeat_started_.compare_exchange_strong(expected, true)) {
        return;
    }

    xTaskCreate([](void* arg) {
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
                if (!client->SendJson("{\"type\":\"ping\"}")) {
                    ESP_LOGW(TAG, "Voice Lab heartbeat ping failed");
                    client->connected_.store(false);
                    client->ScheduleReconnect();
                }
            } else {
                ESP_LOGI(TAG, "Voice Lab heartbeat detected disconnected control channel");
                client->ScheduleReconnect();
            }
        }
    }, "voice_lab_heartbeat", 4096, this, 2, nullptr);
}

void VoiceLabClient::EnsureUsbProvisioningTask() {
    bool expected = false;
    if (!g_usb_provisioning_task_started.compare_exchange_strong(expected, true)) {
        return;
    }

    xTaskCreate([](void*) {
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
                ESP_LOGW(TAG, "Voice Lab USB pairing failed; generate a fresh enrollment code and retry");
            }
        }
    }, "voice_lab_usb", 4096, nullptr, 2, nullptr);
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
    xTaskCreate([](void* arg) {
        auto* client = static_cast<VoiceLabClient*>(arg);
        vTaskDelay(pdMS_TO_TICKS(3000));
        client->reconnect_scheduled_.store(false);
        if (!client->manual_disconnect_.load()) {
            client->ConnectAsync();
        }
        vTaskDelete(nullptr);
    }, "voice_lab_reconn", 4096, this, 2, nullptr);
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
    cJSON_AddItemToObject(root, "capabilities", capabilities);

    cJSON* audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "encoding", "pcm_s16le");
    cJSON_AddNumberToObject(audio, "sampleRate", kAudioSampleRate);
    cJSON_AddNumberToObject(audio, "channels", kAudioChannels);
    cJSON_AddNumberToObject(audio, "frameDurationMs", kAudioFrameDurationMs);
    cJSON_AddStringToObject(audio, "inputSemantics", "mono mixdown from Waveshare ESP32-S3 Audio Board ES7210 input");
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (control_websocket_ == nullptr || !control_websocket_->IsConnected()) {
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
    if (!IsConnected() && !Connect()) {
        return false;
    }

    auto device_key = GetDeviceKey();
    auto token = GetDeviceToken();
    auto audio_url = BuildWebsocketUrl("/api/v1/devices/" + device_key + "/audio");

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ConnectAudioLocked(audio_url, token)) {
            recording_.store(false);
            return false;
        }
        recording_id_ = recording_id;
        recording_mode_ = mode == "counter_file" ? "counter_file" : "meeting_live";
        pending_audio_pcm_.clear();
        audio_sequence_ = 0;
        sample_start_ = 0;
        audio_frames_sent_ = 0;
        audio_bytes_sent_ = 0;
        last_audio_stats_us_ = 0;
        recording_.store(true);
    }

    auto& audio_service = Application::GetInstance().GetAudioService();
    audio_service.EnableVoiceProcessing(false);
    audio_service.EnableWakeWordDetection(false);
    audio_service.EnableExternalCapture(true);
    SetRecordingIndicator(true);
    NotifyRecordingStarted();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "status");
    cJSON_AddStringToObject(root, "mode", recording_mode_.c_str());
    cJSON_AddBoolToObject(root, "recording", true);
    if (!recording_id.empty()) {
        cJSON_AddStringToObject(root, "recordingId", recording_id.c_str());
    }
    auto json = PrintJson(root);
    cJSON_Delete(root);
    SendJson(json);
    return true;
}

bool VoiceLabClient::StopRecording() {
    if (!recording_.exchange(false)) {
        return true;
    }
    recording_mode_ = "idle";

    auto& audio_service = Application::GetInstance().GetAudioService();
    audio_service.EnableExternalCapture(false);
    SetRecordingIndicator(false);
    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        audio_service.EnableWakeWordDetection(true);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (audio_websocket_ != nullptr && audio_websocket_->IsConnected()) {
            while (pending_audio_pcm_.size() >= kPcmSamplesPerFrame &&
                   FlushPendingAudioLocked(true)) {
            }
            ESP_LOGI(TAG, "Voice Lab audio end sent");
            audio_websocket_->Send("{\"type\":\"end\"}");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        CloseAudioLocked();
    }

    SendJson("{\"type\":\"status\",\"mode\":\"idle\",\"recording\":false}");
    return true;
}

bool VoiceLabClient::RequestPlayback(const std::string& url) {
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
    auto profile = GetHardwareProfile();
    if (profile != "waveshare-esp32s3-audio-board" && profile != "esp32-s3-audio-board") {
        return;
    }

    auto* led = static_cast<CircularStrip*>(Board::GetInstance().GetLed());
    if (led == nullptr) {
        return;
    }

    // Waveshare's circular WS2812 ring maps the API red/green channels inversely
    // on this board revision: API red renders physical green, API green renders red.
    StripColor color = recording ? StripColor{0, 0, 32} : StripColor{32, 0, 0};
    led->SetAllColor(color);
}

void VoiceLabClient::NotifyRecordingStarted() {
    Application::GetInstance().Schedule([]() {
        Application::GetInstance().Alert("Voice Lab", "开始会议记录", "recording",
                                         Lang::Sounds::OGG_POPUP);
    });
}

bool VoiceLabClient::SendPcmAudio(std::vector<int16_t>&& pcm) {
    if (!recording_.load()) {
        return false;
    }
    if (pcm.empty() || pcm.size() % kPcmSamplesPerFrame != 0) {
        ESP_LOGW(TAG, "Drop invalid PCM frame: samples=%u",
                 static_cast<unsigned>(pcm.size()));
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (audio_websocket_ == nullptr || !audio_websocket_->IsConnected()) {
        audio_connected_.store(false);
        return false;
    }

    if (!recording_.load()) {
        return false;
    }
    if (pending_audio_pcm_.size() + pcm.size() >
        static_cast<size_t>(2 * kMaxFramesPerPacket * kPcmSamplesPerFrame)) {
        ESP_LOGW(TAG, "Voice Lab PCM buffer full; rejecting capture chunk");
        return false;
    }
    pending_audio_pcm_.insert(pending_audio_pcm_.end(), pcm.begin(), pcm.end());
    while (pending_audio_pcm_.size() >=
           static_cast<size_t>(kMaxFramesPerPacket * kPcmSamplesPerFrame)) {
        if (!FlushPendingAudioLocked(false)) {
            return false;
        }
    }
    return true;
}

bool VoiceLabClient::FlushPendingAudioLocked(bool force) {
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

    const size_t samples_to_send = frames_to_send * kPcmSamplesPerFrame;
    std::vector<int16_t> batch(pending_audio_pcm_.begin(),
                               pending_audio_pcm_.begin() + samples_to_send);
    auto packet = BuildPcmPacket(batch, audio_sequence_, sample_start_);
    if (audio_frames_sent_ == 0) {
        ESP_LOGI(TAG, "Voice Lab PCM first batch ready: frames=%u packet_bytes=%u",
                 static_cast<unsigned>(frames_to_send),
                 static_cast<unsigned>(packet.size()));
    }
    bool sent = audio_websocket_->Send(packet.data(), packet.size(), true);
    if (!sent) {
        ESP_LOGW(TAG, "Failed to send Voice Lab PCM batch: frames=%u samples=%u",
                 static_cast<unsigned>(frames_to_send),
                 static_cast<unsigned>(samples_to_send));
        return false;
    }

    pending_audio_pcm_.erase(pending_audio_pcm_.begin(),
                             pending_audio_pcm_.begin() + samples_to_send);
    audio_sequence_ += frames_to_send;
    sample_start_ += samples_to_send;
    audio_frames_sent_ += frames_to_send;
    audio_bytes_sent_ += samples_to_send * sizeof(int16_t);
    auto now = static_cast<uint64_t>(esp_timer_get_time());
    if (now - last_audio_stats_us_ >= 1000000) {
        ESP_LOGI(TAG, "Voice Lab PCM sent: frames=%llu bytes=%llu sample_start=%llu pending_frames=%u",
                 static_cast<unsigned long long>(audio_frames_sent_),
                 static_cast<unsigned long long>(audio_bytes_sent_),
                 static_cast<unsigned long long>(sample_start_),
                 static_cast<unsigned>(pending_audio_pcm_.size() / kPcmSamplesPerFrame));
        last_audio_stats_us_ = now;
    }
    return true;
}

std::string VoiceLabClient::BuildPcmPacket(const std::vector<int16_t>& pcm,
                                           uint64_t first_sequence,
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

void VoiceLabClient::SendConfigAck(int revision, const std::string& mode,
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
    SendJson(json);
}

void VoiceLabClient::ApplyDesiredConfig(const cJSON* root) {
    auto revision_item = cJSON_GetObjectItem(root, "revision");
    auto mode_item = cJSON_GetObjectItem(root, "mode");
    auto capture_plan = cJSON_GetObjectItem(root, "capturePlan");

    int revision = cJSON_IsNumber(revision_item) ? revision_item->valueint : 0;
    std::string mode = cJSON_IsString(mode_item) ? mode_item->valuestring : "";

    if (mode == "meeting_live" || mode == "counter_file" || mode == "recording" || mode == "capture") {
        std::string applied_mode = mode == "counter_file" ? "counter_file" : "meeting_live";
        bool started = StartRecording("", applied_mode);
        SendConfigAck(revision, applied_mode, capture_plan, started,
                      started ? "" : "failed_to_start_recording");
    } else if (mode == "idle" || mode == "standby" || mode == "off") {
        StopRecording();
        SendConfigAck(revision, "idle", capture_plan, true);
    } else {
        SendConfigAck(revision, mode, capture_plan, false, "unsupported_mode");
    }
}

void VoiceLabClient::HandleJson(const cJSON* root) {
    auto type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        return;
    }

    if (strcmp(type->valuestring, "desired_config") == 0) {
        ApplyDesiredConfig(root);
    } else if (strcmp(type->valuestring, "ping") == 0) {
        SendJson("{\"type\":\"pong\"}");
    } else if (strcmp(type->valuestring, "playback") == 0) {
        auto action = cJSON_GetObjectItem(root, "action");
        auto url = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(action) && strcmp(action->valuestring, "play_url") == 0 &&
            cJSON_IsString(url)) {
            RequestPlayback(url->valuestring);
        } else if (cJSON_IsString(action) && strcmp(action->valuestring, "stop") == 0) {
            StopPlayback();
        }
    }
}

void VoiceLabClient::HandleAudioJson(const cJSON* root) {
    auto type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "ingress_ack") == 0) {
        auto sequence = cJSON_GetObjectItem(root, "sequence");
        auto sample_end = cJSON_GetObjectItem(root, "sampleEnd");
        ESP_LOGI(TAG, "Voice Lab ingress ack: sequence=%lld sample_end=%lld",
                 cJSON_IsNumber(sequence) ? static_cast<long long>(sequence->valuedouble) : -1,
                 cJSON_IsNumber(sample_end) ? static_cast<long long>(sample_end->valuedouble) : -1);
        return;
    }

    auto status = cJSON_GetObjectItem(root, "status");
    if (cJSON_IsString(status)) {
        ESP_LOGI(TAG, "Voice Lab audio status: %s", status->valuestring);
    }
}

std::string VoiceLabClient::GetStatusJson() const {
    auto device_key = GetDeviceKey();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", IsConnected());
    cJSON_AddBoolToObject(root, "audio_connected", audio_connected_.load());
    cJSON_AddBoolToObject(root, "recording", recording_.load());
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
    EnsureUsbProvisioningTask();
    SetRecordingIndicator(false);

    auto& mcp_server = McpServer::GetInstance();

    mcp_server.AddUserOnlyTool("self.voice_lab.configure",
        "Configure ESP-side Voice Lab host, port, TLS, device key, enrollment code, and auto-connect behavior.",
        PropertyList({
            Property("server_host", kPropertyTypeString, std::string(CONFIG_VOICE_LAB_SERVER_HOST)),
            Property("server_port", kPropertyTypeInteger, CONFIG_VOICE_LAB_SERVER_PORT, 1, 65535),
            Property("server_tls", kPropertyTypeBoolean, static_cast<bool>(CONFIG_VOICE_LAB_SERVER_TLS)),
            Property("device_key", kPropertyTypeString, std::string(CONFIG_VOICE_LAB_DEVICE_KEY)),
            Property("hardware_profile", kPropertyTypeString, std::string(CONFIG_VOICE_LAB_HARDWARE_PROFILE)),
            Property("audio_frontend", kPropertyTypeString, std::string(CONFIG_VOICE_LAB_AUDIO_FRONTEND)),
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
            auto enrollment_code = NormalizeEnrollmentCode(properties["enrollment_code"].value<std::string>());

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

    mcp_server.AddUserOnlyTool("self.voice_lab.pair",
        "Exchange a short Voice Lab enrollment code for a device credential and store it in NVS.",
        PropertyList({
            Property("enrollment_code", kPropertyTypeString),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            return VoiceLabClient::GetInstance().PairWithEnrollmentCode(
                properties["enrollment_code"].value<std::string>());
        });

    mcp_server.AddUserOnlyTool("self.voice_lab.unpair",
        "Clear the stored Voice Lab device credential and enrollment code from NVS.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            VoiceLabClient::GetInstance().ClearPairing();
            return true;
        });

    mcp_server.AddUserOnlyTool("self.voice_lab.reset",
        "Reset only Voice Lab NVS settings; Wi-Fi credentials are not changed.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            VoiceLabClient::GetInstance().ResetVoiceLabSettings();
            return true;
        });

    mcp_server.AddUserOnlyTool("self.voice_lab.get_status",
        "Get Voice Lab connection, recording, playback and audio capability status.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return VoiceLabClient::GetInstance().GetStatusJson();
        });

    mcp_server.AddUserOnlyTool("self.voice_lab.connect",
        "Connect to Voice Lab immediately.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return VoiceLabClient::GetInstance().Connect();
        });

    mcp_server.AddUserOnlyTool("self.voice_lab.disconnect",
        "Disconnect from Voice Lab and stop recording.",
        PropertyList(),
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
        "Stop streaming microphone audio to Voice Lab.",
        PropertyList(),
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
        "Stop current Voice Lab playback on the device.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            VoiceLabClient::GetInstance().StopPlayback();
            return true;
        });
}
