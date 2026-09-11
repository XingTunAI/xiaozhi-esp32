/*
 * WiFi Manager Implementation
 */

#include "wifi_manager.h"
#include "ssid_manager.h"
#include "wifi_configuration_ap.h"
#include "wifi_station.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <algorithm>
#include <cstring>

#define TAG "WifiManager"

WifiManager& WifiManager::GetInstance() {
    static WifiManager instance;
    return instance;
}

WifiManager::WifiManager() = default;

WifiManager::~WifiManager() {
    {
        std::lock_guard<std::recursive_mutex> notification_lock(notification_mutex_);
        std::lock_guard<std::mutex> lock(mutex_);
        event_callback_ = nullptr;
        ++station_generation_;
        if (config_window_timer_) {
            esp_timer_stop(config_window_timer_);
            esp_timer_delete(config_window_timer_);
        }
        if (station_active_ && station_) {
            station_->Stop();
        }
        if (config_mode_active_ && config_ap_) {
            config_ap_->Stop();
        }
        station_active_ = false;
        config_mode_active_ = false;
        if (initialized_) {
            esp_wifi_deinit();
        }
        initialized_ = false;
    }
    // Sources are stopped. Never join a worker while holding a lock it needs.
    if (event_worker_) {
        event_worker_stopping_ = true;
        xTaskNotifyGive(event_worker_);
        xSemaphoreTake(event_worker_done_, portMAX_DELAY);
    }
    if (station_events_) {
        vQueueDelete(station_events_);
    }
    if (event_worker_done_) {
        vSemaphoreDelete(event_worker_done_);
    }
}

void WifiManager::NotifyEvent(WifiEvent event, const std::string& data, uint32_t generation) {
    std::lock_guard<std::recursive_mutex> notification_lock(notification_mutex_);
    // Copy callback under lock, invoke without lock to avoid deadlock
    std::function<void(WifiEvent, const std::string&)> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != 0 && (generation != station_generation_.load() || !station_active_)) {
            return;
        }
        callback = event_callback_;
    }
    if (callback) {
        callback(event, data);
    }
}

bool WifiManager::StartEventWorker() {
    station_events_ = xQueueCreate(16, sizeof(StationEvent));
    event_worker_done_ = xSemaphoreCreateBinary();
    if (station_events_ && event_worker_done_ &&
        xTaskCreate(
            [](void* arg) {
                auto* self = static_cast<WifiManager*>(arg);
                self->RunEventWorker();
                xSemaphoreGive(self->event_worker_done_);
                vTaskDelete(nullptr);
            },
            "wifi_events", 4096, this, 2, &event_worker_) == pdPASS) {
        return true;
    }
    if (station_events_) {
        vQueueDelete(station_events_);
        station_events_ = nullptr;
    }
    if (event_worker_done_) {
        vSemaphoreDelete(event_worker_done_);
        event_worker_done_ = nullptr;
    }
    event_worker_ = nullptr;
    return false;
}

void WifiManager::QueueStationEvent(WifiEvent event, const std::string& data, uint32_t generation) {
    if (event_worker_stopping_ || generation != station_generation_.load()) {
        return;
    }
    StationEvent item{};
    item.event = event;
    item.generation = generation;
    std::memcpy(item.data, data.data(), std::min(data.size(), sizeof(item.data) - 1));
    // Only fixed copies/scalars in this short critical section. In particular,
    // never read WifiStation's mutable std::string from the consumer task.
    portENTER_CRITICAL(&snapshot_mux_);
    if (station_snapshot_.generation != generation) {
        const auto sequence = station_snapshot_.sequence;
        station_snapshot_ = {};
        station_snapshot_.sequence = sequence;
        station_snapshot_.generation = generation;
    }
    item.sequence = ++station_snapshot_.sequence;
    if (event == WifiEvent::Connected) {
        station_snapshot_.connected = true;
        std::memcpy(station_snapshot_.ssid, item.data, sizeof(item.data));
    } else if (event == WifiEvent::Disconnected) {
        station_snapshot_.connected = false;
    }
    portEXIT_CRITICAL(&snapshot_mux_);
    if (xQueueSend(station_events_, &item, 0) != pdTRUE &&
        (event == WifiEvent::Connected || event == WifiEvent::Disconnected)) {
        portENTER_CRITICAL(&snapshot_mux_);
        if (station_snapshot_.generation == generation) {
            station_snapshot_.missed_critical = true;
        }
        portEXIT_CRITICAL(&snapshot_mux_);
    }
    xTaskNotifyGive(event_worker_);
}

void WifiManager::RunEventWorker() {
    uint64_t resynchronized_sequence = 0;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (event_worker_stopping_) {
            return;
        }
        StationEvent item{};
        for (unsigned i = 0; i < 16 && !event_worker_stopping_ &&
                             xQueueReceive(station_events_, &item, 0) == pdTRUE;
             ++i) {
            if (item.sequence > resynchronized_sequence) {
                NotifyEvent(item.event, item.data, item.generation);
            }
        }
        StationSnapshot snapshot;
        portENTER_CRITICAL(&snapshot_mux_);
        snapshot = station_snapshot_;
        station_snapshot_.missed_critical = false;
        portEXIT_CRITICAL(&snapshot_mux_);
        if (snapshot.missed_critical && !event_worker_stopping_) {
            resynchronized_sequence = snapshot.sequence;
            // Fail closed on critical overflow: report interruption before the
            // latest connected state, so an unseen disconnect cannot preserve
            // a recording/session. Older queued records cannot undo this sync.
            std::lock_guard<std::recursive_mutex> notification_lock(notification_mutex_);
            if (snapshot.generation == station_generation_.load()) {
                ESP_LOGW(TAG, "WiFi event queue overflow; resynchronizing connection state");
                NotifyEvent(WifiEvent::Disconnected, "event_queue_overflow", snapshot.generation);
                if (snapshot.connected) {
                    NotifyEvent(WifiEvent::Connected, snapshot.ssid, snapshot.generation);
                }
            }
        }
    }
}

bool WifiManager::Initialize(const WifiManagerConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }

    config_ = config;
    ESP_LOGI(TAG, "Initializing...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (config_.customer_mode) {
            ESP_LOGE(TAG, "NVS needs maintenance; preserving customer device identity");
            return false;
        }
        ESP_LOGW(TAG, "Erasing NVS...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Initialize netif
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Netif init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Create event loop
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop create failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Initialize WiFi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        return false;
    }

    station_ = std::make_unique<WifiStation>();
    config_ap_ = std::make_unique<WifiConfigurationAp>();

    // Open provisioning deliberately ignores any legacy NVS AP password.
    // Preserve that key for firmware rollback; do not touch station or device credentials.
    ap_password_.clear();
    if (config_.customer_mode && !config_.open_config_ap && !LoadOrCreateApPassword()) {
        ESP_LOGE(TAG, "Customer AP credential unavailable; refusing open provisioning");
        return false;
    }
    if (config_.config_ap_timeout_seconds > 0) {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = OnConfigWindowTimeout;
        timer_args.arg = this;
        timer_args.dispatch_method = ESP_TIMER_TASK;
        timer_args.name = "wifi_cfg_window";
        if (esp_timer_create(&timer_args, &config_window_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "Could not create configuration window timer");
            return false;
        }
    }

    if (!StartEventWorker()) {
        ESP_LOGE(TAG, "Could not start WiFi event dispatcher");
        if (config_window_timer_) {
            esp_timer_delete(config_window_timer_);
            config_window_timer_ = nullptr;
        }
        config_ap_.reset();
        station_.reset();
        esp_wifi_deinit();
        return false;
    }
    initialized_ = true;
    ESP_LOGI(TAG, "Initialized");
    return true;
}

bool WifiManager::LoadOrCreateApPassword() {
    // Separate from WiFi credentials and Voice Lab identity. Never log this value.
    nvs_handle_t nvs;
    esp_err_t result = nvs_open("wifi_provision", NVS_READWRITE, &nvs);
    if (result != ESP_OK) {
        return false;
    }
    char stored[65] = {};
    size_t size = sizeof(stored);
    result = nvs_get_str(nvs, "ap_password", stored, &size);
    if (result == ESP_OK) {
        ap_password_ = stored;
    }
    // Migrate the earlier 12-character screen password to a speakable code.
    if (result == ESP_ERR_NVS_NOT_FOUND || (result == ESP_OK && ap_password_.length() == 12)) {
        ap_password_.clear();
        ap_password_.reserve(8);
        while (ap_password_.length() < 8) {
            uint8_t random[16];
            esp_fill_random(random, sizeof(random));
            for (uint8_t value : random) {
                // Rejection sampling gives each decimal digit equal probability.
                if (value < 250) {
                    ap_password_.push_back(static_cast<char>('0' + value % 10));
                    if (ap_password_.length() == 8) {
                        break;
                    }
                }
            }
        }
        result = nvs_set_str(nvs, "ap_password", ap_password_.c_str());
        if (result == ESP_OK) {
            result = nvs_commit(nvs);
        }
    }
    nvs_close(nvs);
    if (result != ESP_OK || ap_password_.length() != 8 ||
        ap_password_.find_first_not_of("0123456789") != std::string::npos) {
        ap_password_.clear();
        return false;
    }
    return true;
}

void WifiManager::OnConfigWindowTimeout(void* arg) {
    // HTTP shutdown may wait for an active request. Keep it off the timer task.
    auto* self = static_cast<WifiManager*>(arg);
    const auto created = xTaskCreate(
        [](void* ctx) {
            static_cast<WifiManager*>(ctx)->StopConfigApInternal(true);
            vTaskDelete(nullptr);
        },
        "wifi_cfg_expire", 4096, self, 2, nullptr);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Configuration expiry worker unavailable; retrying");
        esp_timer_start_once(self->config_window_timer_, 1000000ULL);
    }
}

bool WifiManager::IsInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

// ==================== Station Mode ====================

void WifiManager::StartStation() {
    std::lock_guard<std::recursive_mutex> notification_lock(notification_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);

    if (!initialized_) {
        ESP_LOGE(TAG, "Not initialized");
        return;
    }
    if (station_active_) {
        ESP_LOGW(TAG, "Station already active");
        return;
    }

    // Auto-stop config AP if active
    if (config_mode_active_) {
        ESP_LOGI(TAG, "Stopping config AP before starting station");
        if (config_window_timer_) {
            esp_timer_stop(config_window_timer_);
        }
        config_ap_->Stop();
        config_mode_active_ = false;
        // Notify outside lock
        lock.unlock();
        NotifyEvent(WifiEvent::ConfigModeExit);
        lock.lock();
        // The exit callback may already start a station (or a fresh AP).
        // Recursive notification serialization permits that callback safely.
        if (station_active_ || config_mode_active_) {
            return;
        }
    }

    ESP_LOGI(TAG, "Starting station");

    // Apply configuration
    station_->SetScanIntervalRange(config_.station_scan_min_interval_seconds,
                                   config_.station_scan_max_interval_seconds);
    station_->SetFailureRetryCnt(config_.station_failure_retry_cnt);
    station_->SetHostname(config_.station_hostname);

    // ESP event handlers must never wait for mutex_ while a mode transition
    // holds it and unregisters handlers. They only enqueue fixed-size records.
    const uint32_t generation = ++station_generation_;
    station_->OnScanBegin(
        [this, generation]() { QueueStationEvent(WifiEvent::Scanning, "", generation); });
    station_->OnConnect([this, generation](const std::string& ssid) {
        QueueStationEvent(WifiEvent::Connecting, ssid, generation);
    });
    station_->OnConnected([this, generation](const std::string& ssid) {
        QueueStationEvent(WifiEvent::Connected, ssid, generation);
    });
    station_->OnDisconnected([this, generation](int reason) {
        QueueStationEvent(WifiEvent::Disconnected, std::to_string(reason), generation);
    });

    station_->Start();
    station_active_ = true;
}

void WifiManager::StopStation() {
    std::lock_guard<std::recursive_mutex> notification_lock(notification_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);

    if (!station_active_) {
        return;
    }

    ESP_LOGI(TAG, "Stopping station");
    ++station_generation_;
    station_->Stop();
    ESP_LOGI(TAG, "Station stopped");
    station_active_ = false;

    lock.unlock();
    NotifyEvent(WifiEvent::Disconnected);
    lock.lock();
}

bool WifiManager::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return station_active_ && station_ && station_->IsConnected();
}

std::string WifiManager::GetSsid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_)
        return "";
    return station_->GetSsid();
}

std::string WifiManager::GetIpAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_)
        return "";
    return station_->GetIpAddress();
}

int WifiManager::GetRssi() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_ || !station_->IsConnected())
        return 0;
    return station_->GetRssi();
}

int WifiManager::GetChannel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_ || !station_->IsConnected())
        return 0;
    return station_->GetChannel();
}

std::string WifiManager::GetMacAddress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mac_address_.empty()) {
        return mac_address_;
    }

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char buf[18];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
                 mac[4], mac[5]);
        mac_address_ = buf;
    }
    return mac_address_;
}

// ==================== Config AP Mode ====================

void WifiManager::StartConfigAp() {
    std::lock_guard<std::recursive_mutex> notification_lock(notification_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);

    if (!initialized_) {
        ESP_LOGE(TAG, "Not initialized");
        return;
    }
    if (config_mode_active_) {
        ESP_LOGW(TAG, "Config AP already active");
        return;
    }

    // Auto-stop station if active
    if (station_active_) {
        ESP_LOGI(TAG, "Stopping station before starting config AP");
        ++station_generation_;
        station_->Stop();
        station_active_ = false;
        lock.unlock();
        NotifyEvent(WifiEvent::Disconnected);
        lock.lock();
    }

    ESP_LOGI(TAG, "Starting config AP");

    config_ap_->SetSsidPrefix(config_.ssid_prefix);
    config_ap_->SetLanguage(config_.language);
    config_ap_->SetShowOtaConfig(config_.show_ota_config);
    config_ap_->SetShowSleepConfig(config_.show_sleep_config);
    config_ap_->SetCustomerMode(config_.customer_mode);
    config_ap_->SetPassword(ap_password_);

    // Web handler calls this when user submits config
    config_ap_->OnExitRequested([this]() {
        ESP_LOGI(TAG, "Config exit requested from web");
        StopConfigAp();
    });

    config_ap_->CancelTimeout();
    config_ap_->Start();
    config_mode_active_ = true;
    if (config_window_timer_) {
        const uint64_t window_us = config_.config_ap_timeout_seconds * 1000000ULL;
        config_window_deadline_us_ = esp_timer_get_time() + window_us;
        ESP_ERROR_CHECK(esp_timer_start_once(config_window_timer_, window_us));
    }

    lock.unlock();
    NotifyEvent(WifiEvent::ConfigModeEnter);
    lock.lock();
}

void WifiManager::StopConfigAp() { StopConfigApInternal(false); }

void WifiManager::StopConfigApInternal(bool expired) {
    std::lock_guard<std::recursive_mutex> notification_lock(notification_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);

    if (!config_mode_active_) {
        return;
    }
    // A queued timer worker must not close a newly opened configuration window.
    if (expired && esp_timer_get_time() < config_window_deadline_us_) {
        return;
    }
    if (expired && config_.customer_mode) {
        if (!config_ap_->TryBeginTimeout()) {
            // A timeout cannot interrupt an HTTP submission or WiFi validation.
            config_window_deadline_us_ = esp_timer_get_time() + 30000000LL;
            esp_timer_start_once(config_window_timer_, 30000000ULL);
            return;
        }
        if (SsidManager::GetInstance().GetSsidList().empty()) {
            config_ap_->CancelTimeout();
            ESP_LOGI(TAG, "No saved WiFi; retaining configuration AP");
            return;
        }
    }
    if (config_window_timer_) {
        esp_timer_stop(config_window_timer_);
    }

    ESP_LOGI(TAG, "Stopping config AP");
    config_ap_->Stop();
    config_mode_active_ = false;

    lock.unlock();
    NotifyEvent(expired ? WifiEvent::ConfigModeExpired : WifiEvent::ConfigModeExit);
    lock.lock();
}

bool WifiManager::IsConfigMode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_mode_active_;
}

std::string WifiManager::GetApSsid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_mode_active_ || !config_ap_)
        return "";
    return config_ap_->GetSsid();
}

std::string WifiManager::GetApWebUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_mode_active_ || !config_ap_)
        return "";
    return config_ap_->GetWebServerUrl();
}

std::string WifiManager::GetApPassword() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ap_password_;
}

// ==================== Power ====================

void WifiManager::SetPowerSaveLevel(WifiPowerSaveLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!station_active_ || !station_) {
        return;
    }
    station_->SetPowerSaveLevel(level);
}

// ==================== Event ====================

void WifiManager::SetEventCallback(std::function<void(WifiEvent, const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_callback_ = std::move(callback);
}
