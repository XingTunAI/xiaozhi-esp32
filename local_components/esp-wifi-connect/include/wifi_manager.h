/*
 * WiFi Manager - Unified WiFi connection management
 *
 * Thread Safety:
 * - All public methods are thread-safe (protected by internal mutex)
 * - Station callbacks run on a bounded dispatch worker, never the ESP event loop.
 * - Mode transitions and callbacks share a recursive serialization boundary.
 *
 * Usage:
 *   auto& wifi = WifiManager::GetInstance();
 *
 *   EventGroupHandle_t events = xEventGroupCreate();
 *   wifi.SetEventCallback([events](WifiEvent e) {
 *       if (e == WifiEvent::Connected) xEventGroupSetBits(events, BIT0);
 *       if (e == WifiEvent::ConfigModeExit) xEventGroupSetBits(events, BIT1);
 *   });
 *
 *   wifi.Initialize(config);
 *   wifi.StartStation();
 *   xEventGroupWaitBits(events, BIT0 | BIT1, pdTRUE, pdFALSE, portMAX_DELAY);
 */

#ifndef _WIFI_MANAGER_H_
#define _WIFI_MANAGER_H_

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "wifi_station.h"

class WifiStation;
class WifiConfigurationAp;

// WiFi events
enum class WifiEvent {
    Scanning,           // Started scanning for networks
    Connecting,         // Connecting to network (call GetSsid() for target)
    Connected,          // Successfully connected
    Disconnected,       // Disconnected from network
    ConfigModeEnter,    // Entered config AP mode
    ConfigModeExit,     // Exited config AP mode
    ConfigModeExpired,  // Configuration window elapsed; board may retry saved WiFi
};

// Configuration
struct WifiManagerConfig {
    std::string ssid_prefix = "ESP32";  // AP mode SSID prefix
    std::string language = "zh-CN";     // Web UI language

    // Station mode scan interval with exponential backoff
    int station_scan_min_interval_seconds = 10;   // Initial scan interval (fast retry)
    int station_scan_max_interval_seconds = 300;  // Maximum scan interval (5 minutes)
    std::string station_hostname;                 // Optional DHCP hostname for station mode

    // How many times to retry the strongest same-SSID AP before falling back to
    // a weaker one (requires WIFI_ALL_CHANNEL_SCAN, which is the default when
    // remember_bssid is off). 0 = driver default (one attempt only).
    uint8_t station_failure_retry_cnt = 3;

    // Whether to show the OTA URL field in the config portal advanced tab.
    // Defaults to false so the field is hidden unless explicitly enabled.
    bool show_ota_config = false;

    // Whether to show the sleep mode toggle in the config portal advanced tab.
    bool show_sleep_config = false;
    // Customer mode serves only WiFi setup and cannot modify service/identity settings.
    bool customer_mode = false;
    // Explicit board opt-in; existing customer boards retain their protected AP.
    bool open_config_ap = false;
    // Zero retains the legacy unlimited window. Customer boards choose a bound.
    uint32_t config_ap_timeout_seconds = 0;
};

/**
 * WifiManager - Singleton for WiFi management
 */
class WifiManager {
public:
    static WifiManager& GetInstance();

    // ==================== Lifecycle ====================

    bool Initialize(const WifiManagerConfig& config = WifiManagerConfig{});
    bool IsInitialized() const;

    // ==================== Station Mode ====================

    void StartStation();  // Non-blocking, auto-stops config AP if active
    void StopStation();   // Non-blocking

    bool IsConnected() const;
    std::string GetSsid() const;
    std::string GetIpAddress() const;
    int GetRssi() const;
    int GetChannel() const;
    std::string GetMacAddress() const;

    // ==================== Config AP Mode ====================

    void StartConfigAp();  // Non-blocking, auto-stops station if active
    void StopConfigAp();   // Non-blocking

    bool IsConfigMode() const;
    std::string GetApSsid() const;
    std::string GetApWebUrl() const;
    // Physical spoken prompt/internal provisioning only. Never expose via network status.
    std::string GetApPassword() const;

    // ==================== Power ====================

    void SetPowerSaveLevel(WifiPowerSaveLevel level);

    // ==================== Event ====================

    void SetEventCallback(std::function<void(WifiEvent, const std::string&)> callback);

    const WifiManagerConfig& GetConfig() const { return config_; }

    WifiManager(const WifiManager&) = delete;
    WifiManager& operator=(const WifiManager&) = delete;

private:
    WifiManager();
    ~WifiManager();

    void NotifyEvent(WifiEvent event, const std::string& data = "", uint32_t generation = 0);
    struct StationEvent {
        WifiEvent event;
        uint32_t generation;
        uint64_t sequence;
        char data[33];
    };
    struct StationSnapshot {
        uint32_t generation = 0;
        uint64_t sequence = 0;
        bool connected = false;
        bool missed_critical = false;
        char ssid[33] = {};
    };
    bool StartEventWorker();
    void QueueStationEvent(WifiEvent event, const std::string& data, uint32_t generation);
    void RunEventWorker();
    bool LoadOrCreateApPassword();
    void StopConfigApInternal(bool expired);
    static void OnConfigWindowTimeout(void* arg);

    WifiManagerConfig config_;
    std::unique_ptr<WifiStation> station_;
    std::unique_ptr<WifiConfigurationAp> config_ap_;

    mutable std::mutex mutex_;
    // Always acquire this before mutex_ when both are needed. ESP callbacks
    // only enqueue and never take either lock, including during unregister.
    std::recursive_mutex notification_mutex_;
    QueueHandle_t station_events_ = nullptr;
    SemaphoreHandle_t event_worker_done_ = nullptr;
    TaskHandle_t event_worker_ = nullptr;
    std::atomic<bool> event_worker_stopping_{false};
    std::atomic<uint32_t> station_generation_{0};
    portMUX_TYPE snapshot_mux_ = portMUX_INITIALIZER_UNLOCKED;
    StationSnapshot station_snapshot_;
    bool initialized_ = false;
    bool station_active_ = false;
    bool config_mode_active_ = false;
    esp_timer_handle_t config_window_timer_ = nullptr;
    int64_t config_window_deadline_us_ = 0;
    std::string ap_password_;

    std::function<void(WifiEvent, const std::string&)> event_callback_;
    mutable std::string mac_address_;
};

#endif  // _WIFI_MANAGER_H_
