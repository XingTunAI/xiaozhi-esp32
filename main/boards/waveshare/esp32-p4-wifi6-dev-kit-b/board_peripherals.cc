#include "board_peripherals.h"
#include "c6_update.h"
#include "voice_lab_client.h"

#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_eth.h>
#include <esp_eth_netif_glue.h>
#include <esp_eth_phy_ip101.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_hosted.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_vfs_fat.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <hal/usb_wrap_ll.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <sdmmc_cmd.h>
#include <ssid_manager.h>
#include <usb/uac2_host.h>
#include <usb/uac_host.h>
#include <usb/usb_host.h>
#include <algorithm>
#include <cstring>
#include <mutex>

namespace {
constexpr char TAG[] = "P4Peripherals";
void LogResources(const char* stage) {
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    ESP_LOGI(TAG,
             "Resources %s: SRAM free=%u min=%u DMA free=%u largest=%u PSRAM free=%u stack_min=%u",
             stage, static_cast<unsigned>(heap_caps_get_free_size(internal)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(internal)),
             static_cast<unsigned>(heap_caps_get_free_size(dma)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(dma)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}
EventGroupHandle_t wifi_events = nullptr;
std::mutex status_mutex;
BoardPeripheralStatus status;
std::function<void(bool)> network_callback;
bool network_connected = false;
void PublishNetworkState() {
    bool connected;
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        connected = status.wifi_ip != "--" || status.ethernet_ip != "--";
        if (connected == network_connected)
            return;
        network_connected = connected;
    }
    if (network_callback)
        network_callback(connected);
}
struct BoardCommand {
    int type;
    char ssid[33];
    char password[65];
    char update_url[160];
};
QueueHandle_t commands = nullptr;
bool wifi_initialized = false;
bool wifi_reconnect = false;
bool sd_mounted = false;
bool sd_recovery_blocked = false;
sdmmc_card_t* mounted_card = nullptr;
sd_pwr_ctrl_handle_t mounted_sd_power = nullptr;
std::string IpText(const esp_ip4_addr_t& ip) {
    char value[16];
    snprintf(value, sizeof(value), IPSTR, IP2STR(&ip));
    return value;
}
struct UacAttachment {
    uint8_t address;
    uint8_t interface;
    uac_host_driver_event_t event;
    uac_host_device_handle_t disconnected = nullptr;
};
QueueHandle_t uac_attachments = nullptr;

// Run in the peripheral worker, never in the event loop or audio tasks.
void LogWifiLink() {
    if (!wifi_initialized)
        return;
    wifi_ap_record_t ap{};
    const auto err = esp_wifi_sta_get_ap_info(&ap);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi link: rssi=%d channel=%u bssid=" MACSTR, ap.rssi, ap.primary,
                 MAC2STR(ap.bssid));
    } else {
        ESP_LOGW(TAG, "Wi-Fi link query: %s", esp_err_to_name(err));
    }
}

void WifiScanEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE)
        xEventGroupSetBits(wifi_events, BIT0);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            status.wifi_ip = IpText(event->ip_info.ip);
            status.wifi = "已连接";
        }
        xEventGroupSetBits(wifi_events, BIT1);
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(data);
        ESP_LOGW(TAG, "Wi-Fi disconnected: reason=%u", event ? event->reason : 0);
        std::lock_guard<std::mutex> lock(status_mutex);
        status.wifi_ip = "--";
        status.wifi = "连接已断开";
    }
    PublishNetworkState();
}

esp_err_t InitializeWifi() {
    wifi_events = xEventGroupCreate();
    if (!wifi_events)
        return ESP_ERR_NO_MEM;
    if (!esp_netif_create_default_wifi_sta())
        return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = false;
    auto err = esp_wifi_init(&init);
    if (err != ESP_OK)
        return err;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, WifiScanEvent, nullptr);
    if (err != ESP_OK)
        return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, WifiScanEvent, nullptr);
    if (err != ESP_OK)
        return err;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK)
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK)
        err = esp_wifi_start();
    if (err == ESP_OK) {
        wifi_initialized = true;
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_hosted_coprocessor_fwver_t version = {};
        const int result = esp_hosted_get_coprocessor_fwversion(&version);
        ESP_LOGI(TAG, "C6 firmware query: %s, version=%u.%u.%u", esp_err_to_name(result),
                 static_cast<unsigned>(version.major1), static_cast<unsigned>(version.minor1),
                 static_cast<unsigned>(version.patch1));
        std::lock_guard<std::mutex> lock(status_mutex);
        status.c6_version = result == ESP_OK && (version.major1 || version.minor1 || version.patch1)
                                ? std::to_string(version.major1) + "." +
                                      std::to_string(version.minor1) + "." +
                                      std::to_string(version.patch1)
                                : "未提供有效版本";
    }
    return err;
}

esp_err_t CheckWifiRadio() {
    if (!wifi_initialized)
        return ESP_ERR_INVALID_STATE;
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        status.wifi_busy = true;
        status.wifi = "正在扫描";
    }
    xEventGroupClearBits(wifi_events, BIT0);
    auto err = esp_wifi_scan_start(nullptr, false);
    if (err == ESP_OK &&
        !(xEventGroupWaitBits(wifi_events, BIT0, pdTRUE, pdFALSE, pdMS_TO_TICKS(30000)) & BIT0)) {
        err = ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) {
        wifi_ap_record_t records[20] = {};
        uint16_t count = 20;
        err = esp_wifi_scan_get_ap_records(&count, records);
        std::lock_guard<std::mutex> lock(status_mutex);
        status.networks.clear();
        for (unsigned i = 0; err == ESP_OK && i < count; ++i) {
            std::string ssid(reinterpret_cast<char*>(records[i].ssid),
                             strnlen(reinterpret_cast<char*>(records[i].ssid), 32));
            if (ssid.empty() || ssid.find_first_of("\r\n") != std::string::npos)
                continue;
            if (std::find(status.networks.begin(), status.networks.end(), ssid) ==
                status.networks.end())
                status.networks.push_back(ssid);
        }
        ++status.scan_revision;
        ESP_LOGI(TAG, "C6 scan completed: %u APs", count);
    }
    esp_wifi_clear_ap_list();
    esp_wifi_scan_stop();
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        status.wifi_busy = false;
        status.wifi =
            err == ESP_OK ? (status.wifi_ip == "--" ? "扫描完成" : "已连接") : "扫描失败，请重试";
    }
    return err;
}

void ConnectWifi(const BoardCommand& command, bool retry_saved = false) {
    if (!wifi_initialized)
        return;
    wifi_reconnect = false;
    ESP_LOGI(TAG, "Wi-Fi local disconnect requested: configure; saved=%d", retry_saved);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
    xEventGroupClearBits(wifi_events, BIT1);
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        status.wifi = "正在连接";
        status.wifi_busy = true;
        status.wifi_ssid = command.ssid;
        status.wifi_ip = "--";
    }
    wifi_config_t config = {};
    memcpy(config.sta.ssid, command.ssid, strlen(command.ssid));
    memcpy(config.sta.password, command.password, strlen(command.password));
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    auto err = esp_wifi_set_config(WIFI_IF_STA, &config);
    memset(config.sta.password, 0, sizeof(config.sta.password));
    if (err == ESP_OK)
        err = esp_wifi_connect();
    // Saved credentials must survive a slow AP/DHCP startup. Interactive attempts
    // still report failure without persisting unverified credentials.
    wifi_reconnect = retry_saved && err == ESP_OK;
    if (err == ESP_OK &&
        !(xEventGroupWaitBits(wifi_events, BIT1, pdTRUE, pdFALSE, pdMS_TO_TICKS(25000)) & BIT1))
        err = ESP_ERR_TIMEOUT;
    if (err == ESP_OK) {
        wifi_reconnect = true;
        SsidManager::GetInstance().AddSsid(command.ssid, command.password);
    } else if (!wifi_reconnect) {
        ESP_LOGI(TAG, "Wi-Fi local disconnect requested: connection attempt failed");
        esp_wifi_disconnect();
    }
    ESP_LOGI(TAG, "Wi-Fi connection attempt: %s; retry_saved=%d", esp_err_to_name(err),
             wifi_reconnect);
    std::lock_guard<std::mutex> lock(status_mutex);
    status.wifi_busy = false;
    status.wifi = err == ESP_OK
                      ? "已连接，配置已保存"
                      : (wifi_reconnect ? "连接超时，正在自动重试" : "连接失败，请检查密码或信号");
}

void EthernetEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        const auto* event = static_cast<ip_event_got_ip_t*>(data);
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            status.ethernet = "已连接";
            status.ethernet_ip = IpText(event->ip_info.ip);
            status.gateway = IpText(event->ip_info.gw);
        }
        ESP_LOGI(TAG, "Ethernet DHCP: " IPSTR " gateway " IPSTR, IP2STR(&event->ip_info.ip),
                 IP2STR(&event->ip_info.gw));
    } else if (base == ETH_EVENT) {
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            if (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP) {
                status.ethernet = "网线未连接";
                status.ethernet_ip = "--";
                status.gateway = "--";
            } else if (id == ETHERNET_EVENT_CONNECTED)
                status.ethernet = "正在获取地址";
        }
        ESP_LOGI(TAG, "Ethernet event=%ld (link-up=%d link-down=%d)", static_cast<long>(id),
                 ETHERNET_EVENT_CONNECTED, ETHERNET_EVENT_DISCONNECTED);
    }
    PublishNetworkState();
}

esp_err_t StartEthernet() {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK)
        return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return err;
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = 31;
    emac_config.smi_gpio.mdio_num = 52;
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = 51;
    auto* mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    auto* phy = esp_eth_phy_new_ip101(&phy_config);
    if (!mac || !phy) {
        if (mac)
            mac->del(mac);
        if (phy)
            phy->del(phy);
        return ESP_ERR_NO_MEM;
    }
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t handle = nullptr;
    err = esp_eth_driver_install(&config, &handle);
    if (err != ESP_OK) {
        mac->del(mac);
        phy->del(phy);
        return err;
    }
    uint8_t address[6];
    err = esp_read_mac(address, ESP_MAC_ETH);
    if (err == ESP_OK)
        err = esp_eth_ioctl(handle, ETH_CMD_S_MAC_ADDR, address);
    esp_netif_config_t nc = ESP_NETIF_DEFAULT_ETH();
    esp_netif_inherent_config_t inherent = *nc.base;
    inherent.route_prio = 150;
    nc.base = &inherent;
    auto* netif = err == ESP_OK ? esp_netif_new(&nc) : nullptr;
    auto glue = netif ? esp_eth_new_netif_glue(handle) : nullptr;
    if (!netif || !glue)
        err = ESP_ERR_NO_MEM;
    if (err == ESP_OK)
        err = esp_netif_attach(netif, glue);
    if (err == ESP_OK)
        err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, EthernetEvent, nullptr);
    if (err == ESP_OK)
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, EthernetEvent, nullptr);
    if (err == ESP_OK)
        err = esp_eth_start(handle);
    if (err != ESP_OK) {
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, EthernetEvent);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, EthernetEvent);
        if (glue)
            esp_eth_del_netif_glue(glue);
        if (netif)
            esp_netif_destroy(netif);
        esp_eth_driver_uninstall(handle);
        mac->del(mac);
        phy->del(phy);
    }
    return err;
}

void UacDeviceEvent(uac_host_device_handle_t handle, uac_host_device_event_t event, void*) {
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "UAC interface disconnected");
        UacAttachment attachment{};
        attachment.disconnected = handle;
        if (xQueueSendToFront(uac_attachments, &attachment, 0) != pdTRUE) {
            ESP_LOGE(TAG, "UAC disconnect queue full; interface cleanup pending until reboot");
        }
    } else if (event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
        ESP_LOGW(TAG, "UAC transfer error");
    }
}

void OpenUacInterface(uint8_t address, uint8_t interface, uac_host_driver_event_t event) {
    uac_host_device_config_t config = {};
    config.addr = address;
    config.iface_num = interface;
    config.buffer_size = 4096;
    config.buffer_threshold = 1024;
    config.callback = UacDeviceEvent;
    uac_host_device_handle_t handle = nullptr;
    auto err = uac_host_device_open(&config, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UAC open: %s", esp_err_to_name(err));
        return;
    }
    uac_host_dev_info_t info = {};
    err = uac_host_get_device_info(handle, &info);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "UAC %s VID=%04x PID=%04x interface=%u alternatives=%u; stream stopped",
                 event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED ? "microphone" : "speaker", info.VID,
                 info.PID, interface, info.iface_alt_num);
        for (unsigned alt = 1; alt <= info.iface_alt_num; ++alt) {
            uac_host_dev_alt_param_t format = {};
            if (uac_host_get_device_alt_param(handle, alt, &format) != ESP_OK)
                continue;
            ESP_LOGI(TAG, "UAC alt=%u channels=%u bits=%u format=%u frequency_entries=%u", alt,
                     format.channels, format.bit_resolution, format.format,
                     format.sample_freq_type);
            if (format.sample_freq_type == 0) {
                ESP_LOGI(TAG, "UAC frequency range=%lu..%lu Hz",
                         static_cast<unsigned long>(format.sample_freq_lower),
                         static_cast<unsigned long>(format.sample_freq_upper));
            } else {
                for (unsigned i = 0; i < format.sample_freq_type && i < UAC_FREQ_NUM_MAX; ++i) {
                    ESP_LOGI(TAG, "UAC frequency=%lu Hz",
                             static_cast<unsigned long>(format.sample_freq[i]));
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "UAC descriptor: %s", esp_err_to_name(err));
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        ++status.audio_interfaces;
    }
    // Keep the interface open for disconnect notification; do not start a mic
    // or select an audio format before checking the attached device's support.
}

void UacConnected(uint8_t address, uint8_t interface, uac_host_driver_event_t event, void*) {
    UacAttachment attachment{address, interface, event};
    if (xQueueSend(uac_attachments, &attachment, 0) != pdTRUE) {
        ESP_LOGW(TAG, "UAC attachment queue full; reconnect device to retry");
    }
}

void UacAttachments(void*) {
    UacAttachment attachment;
    while (xQueueReceive(uac_attachments, &attachment, portMAX_DELAY) == pdTRUE) {
        if (attachment.disconnected) {
            const auto err = uac_host_device_close(attachment.disconnected);
            if (err != ESP_OK)
                ESP_LOGW(TAG, "UAC close: %s", esp_err_to_name(err));
            std::lock_guard<std::mutex> lock(status_mutex);
            if (status.audio_interfaces)
                --status.audio_interfaces;
        } else {
            OpenUacInterface(attachment.address, attachment.interface, attachment.event);
        }
    }
    vTaskDelete(nullptr);
}

void UsbEvents(void*) {
    while (true) {
        uint32_t flags = 0;
        const auto err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "USB event loop: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            usb_host_device_free_all();
    }
}

struct Uac2Attachment {
    uint8_t address;
    uint8_t interface;
    uac2_host_driver_event_t event;
    uac2_host_device_handle_t disconnected;
};
QueueHandle_t uac2_attachments = nullptr;
std::mutex capture_mutex;
uac2_host_device_handle_t capture_handle = nullptr;
bool capture_started = false;
// Retain partial USB reads until a complete 20 ms application frame is available.
int16_t capture_pcm[640];
uint32_t capture_bytes = 0;

void Uac2DeviceEvent(uac2_host_device_handle_t handle, uac2_host_device_event_t event, void*) {
    if (event == UAC2_HOST_DEVICE_EVENT_DISCONNECTED) {
        Uac2Attachment item{};
        item.disconnected = handle;
        if (xQueueSendToFront(uac2_attachments, &item, 0) != pdTRUE)
            ESP_LOGE(TAG, "UAC2 disconnect queue full");
    }
}

void Uac2Connected(uint8_t address, uint8_t interface, uac2_host_driver_event_t event, void*) {
    Uac2Attachment item{address, interface, event, nullptr};
    if (xQueueSend(uac2_attachments, &item, 0) != pdTRUE)
        ESP_LOGW(TAG, "UAC2 attachment queue full");
}

void Uac2Attachments(void*) {
    Uac2Attachment item;
    while (xQueueReceive(uac2_attachments, &item, portMAX_DELAY) == pdTRUE) {
        if (item.disconnected) {
            std::lock_guard<std::mutex> capture_lock(capture_mutex);
            if (capture_handle == item.disconnected) {
                capture_handle = nullptr;
                capture_started = false;
                capture_bytes = 0;
            }
            auto err = uac2_host_device_close(item.disconnected);
            ESP_LOGI(TAG, "UAC2 disconnect close: %s", esp_err_to_name(err));
            std::lock_guard<std::mutex> lock(status_mutex);
            if (status.audio_interfaces)
                --status.audio_interfaces;
            continue;
        }
        uac2_host_device_config_t config{};
        config.addr = item.address;
        config.iface_num = item.interface;
        config.buffer_size = 8192;
        config.callback = Uac2DeviceEvent;
        uac2_host_device_handle_t handle = nullptr;
        auto err = uac2_host_device_open(&config, &handle);
        ESP_LOGI(TAG, "UAC2 %s address=%u interface=%u open: %s",
                 item.event == UAC2_HOST_DRIVER_EVENT_RX_CONNECTED ? "microphone" : "speaker",
                 item.address, item.interface, esp_err_to_name(err));
        if (err != ESP_OK)
            continue;
        uac2_host_device_print_info(handle);
        uint32_t rate = 0;
        err = uac2_host_device_get_sample_rate(handle, &rate);
        ESP_LOGI(TAG, "UAC2 current clock: %lu Hz (%s)", static_cast<unsigned long>(rate),
                 esp_err_to_name(err));
        if (err == ESP_OK && rate == 16000 && item.event == UAC2_HOST_DRIVER_EVENT_RX_CONNECTED) {
            uac2_host_dev_alt_param_t alt{};
            if (uac2_host_get_device_alt_param(handle, 1, &alt) == ESP_OK && alt.channels == 2 &&
                alt.bit_resolution == 16 && alt.sub_slot_size == 2) {
                std::lock_guard<std::mutex> capture_lock(capture_mutex);
                if (!capture_handle) {
                    capture_handle = handle;
                    ESP_LOGI(
                        TAG,
                        "Voice Lab USB input ready: 16000 Hz stereo PCM16, selecting channel 0");
                }
            } else {
                ESP_LOGW(TAG, "USB input format is not supported by the Voice Lab capture adapter");
            }
        }
        std::lock_guard<std::mutex> lock(status_mutex);
        ++status.audio_interfaces;
    }
    vTaskDelete(nullptr);
}

esp_err_t StartUsbAudio() {
    ESP_LOGI(TAG, "USB memory: internal=%u DMA=%u largest DMA=%u PSRAM=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
             static_cast<unsigned>(
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    usb_host_config_t host = {};
    host.intr_flags = ESP_INTR_FLAG_LEVEL1;
    host.peripheral_map = BIT0 | BIT1;
    auto err = usb_host_install(&host);
    if (err != ESP_OK)
        return err;
    // H2 uses FSLS PHY 0 (GPIO24 D-, GPIO25 D+). Keep the HS USB-A
    // controller enabled; route FS OTG to H2 in software, without eFuse writes.
    // Apply after the host library initializes both PHYs, before servicing events.
    usb_wrap_ll_phy_select(&USB_WRAP, 0);
    err = gpio_set_drive_capability(GPIO_NUM_24, GPIO_DRIVE_CAP_3);
    if (err == ESP_OK)
        err = gpio_set_drive_capability(GPIO_NUM_25, GPIO_DRIVE_CAP_3);
    if (err != ESP_OK) {
        usb_host_uninstall();
        return err;
    }
    ESP_LOGI(TAG, "USB dual host: HS USB-A + FS H2 (GPIO24 D-, GPIO25 D+)");
    if (xTaskCreate(UsbEvents, "usb_events", 4096, nullptr, 5, nullptr) != pdPASS) {
        usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }
    uac_host_driver_config_t uac = {};
    uac_attachments = xQueueCreate(32, sizeof(UacAttachment));
    if (!uac_attachments)
        return ESP_ERR_NO_MEM;
    if (xTaskCreate(UacAttachments, "uac_attach", 4096, nullptr, 4, nullptr) != pdPASS) {
        vQueueDelete(uac_attachments);
        uac_attachments = nullptr;
        return ESP_ERR_NO_MEM;
    }
    uac.create_background_task = true;
    uac.task_priority = 5;
    uac.stack_size = 4096;
    uac.core_id = tskNO_AFFINITY;
    uac.callback = UacConnected;
    err = uac_host_install(&uac);
    if (err != ESP_OK)
        return err;
    uac2_attachments = xQueueCreate(16, sizeof(Uac2Attachment));
    if (!uac2_attachments)
        return ESP_ERR_NO_MEM;
    if (xTaskCreate(Uac2Attachments, "uac2_attach", 6144, nullptr, 4, nullptr) != pdPASS) {
        vQueueDelete(uac2_attachments);
        uac2_attachments = nullptr;
        return ESP_ERR_NO_MEM;
    }
    uac2_host_driver_config_t uac2{};
    uac2.create_background_task = true;
    uac2.task_priority = 5;
    uac2.stack_size = 6144;
    uac2.core_id = tskNO_AFFINITY;
    uac2.callback = Uac2Connected;
    return uac2_host_install(&uac2);
}

esp_err_t StartSdCard(bool force = false, unsigned attempt = 0) {
    if (sd_recovery_blocked)
        return ESP_ERR_INVALID_STATE;
    if (sd_mounted) {
        if (!force) {
            void* sector = heap_caps_aligned_alloc(64, 512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            if (!sector)
                return ESP_ERR_NO_MEM;
            const auto read = sdmmc_read_sectors(mounted_card, sector, 0, 1);
            free(sector);
            if (read == ESP_OK)
                return ESP_OK;
        }
        const auto unmount = esp_vfs_fat_sdcard_unmount("/sdcard", mounted_card);
        // The VFS unmount can free its context/card even when path removal fails.
        mounted_card = nullptr;
        sd_mounted = false;
        if (unmount != ESP_OK) {
            sd_recovery_blocked = true;
            std::lock_guard<std::mutex> lock(status_mutex);
            status.storage = "卡片不可读，请重启后重新检测";
            return unmount;
        }
        sd_pwr_ctrl_del_on_chip_ldo(mounted_sd_power);
        mounted_sd_power = nullptr;
        mounted_card = nullptr;
        sd_mounted = false;
    }
    // Rev1.2: SD power switch is active-low. SD uses slot 0, C6 uses slot 1.
    gpio_set_level(GPIO_NUM_45, 1);
    gpio_set_direction(GPIO_NUM_45, GPIO_MODE_OUTPUT);
    // Slot has been released before changing power; no global host deinit.
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(GPIO_NUM_45, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    sd_pwr_ctrl_ldo_config_t power = {};
    power.ldo_chan_id = 4;
    sd_pwr_ctrl_handle_t power_handle = nullptr;
    auto err = sd_pwr_ctrl_new_on_chip_ldo(&power, &power_handle);
    if (err != ESP_OK)
        return err;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = 0;
    host.max_freq_khz = attempt ? SDMMC_FREQ_PROBING : SDMMC_FREQ_DEFAULT;
    host.flags |= SDMMC_HOST_FLAG_DEINIT_ARG;
    host.deinit_p = sdmmc_host_deinit_slot;
    host.pwr_ctrl_handle = power_handle;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = GPIO_NUM_43;
    slot.cmd = GPIO_NUM_44;
    slot.d0 = GPIO_NUM_39;
    slot.d1 = GPIO_NUM_40;
    slot.d2 = GPIO_NUM_41;
    slot.d3 = GPIO_NUM_42;
    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files = 8;  // Independent writer, uploader and directory/manifest operations.
    mount.allocation_unit_size = 16 * 1024;
    sdmmc_card_t* card = nullptr;
    err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount, &card);
    if (err != ESP_OK) {
        sd_pwr_ctrl_del_on_chip_ldo(power_handle);
        gpio_set_level(GPIO_NUM_45, 1);
        ESP_LOGW(TAG, "SD mount attempt %u failed: %s; TF slot 0 only", attempt + 1,
                 esp_err_to_name(err));
        if (attempt == 0)
            return StartSdCard(true, 1);
        std::lock_guard<std::mutex> lock(status_mutex);
        status.storage = std::string("初始化失败: ") + esp_err_to_name(err);
        return err;
    }
    sd_mounted = true;
    mounted_card = card;
    mounted_sd_power = power_handle;
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        const uint64_t mb =
            static_cast<uint64_t>(card->csd.capacity) * card->csd.sector_size / (1024 * 1024);
        status.storage = "已挂载，容量 " + std::to_string(mb) + " MB";
    }
    ESP_LOGI(TAG, "SD card mounted; sectors=%lu sector_size=%d; no test files written",
             static_cast<unsigned long>(card->csd.capacity), card->csd.sector_size);
    return ESP_OK;
}

void RecoverSdCard() {
    bool attempted = false;
    const bool ready = VoiceLabClient::GetInstance().MaintainRecordingStorage([&]() {
        attempted = true;
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            status.storage = "正在初始化";
            status.storage_action = "正在检测";
        }
        const auto err = StartSdCard(true);
        ESP_LOGI(TAG, "SD recovery finished: %s", esp_err_to_name(err));
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            status.storage_action = err == ESP_OK ? "检测完成" : "检测失败";
            if (err != ESP_OK && status.storage == "正在初始化")
                status.storage = std::string("初始化失败: ") + esp_err_to_name(err);
        }
        return err == ESP_OK;
    });
    if (!ready && !attempted) {
        ESP_LOGW(TAG, "SD recovery deferred: recording/start/stop or archive I/O busy");
        std::lock_guard<std::mutex> lock(status_mutex);
        status.storage_action = "正在录音或读写，请稍后重试";
        // Preserve the mount state: declining a recovery is not a card failure.
    }
}

void Initialize(void*) {
    auto err = StartEthernet();
    ESP_LOGI(TAG, "IP101 initialization: %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        std::lock_guard<std::mutex> lock(status_mutex);
        status.ethernet = "初始化失败";
    }
    err = StartUsbAudio();
    ESP_LOGI(TAG, "USB host / UAC1+UAC2 initialization: %s", esp_err_to_name(err));
    LogResources("usb-init");
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        status.usb = err == ESP_OK ? "已就绪" : "初始化失败";
    }
    // Optional, never formats a card. Discover pending archives after reboot.
    err = StartSdCard();
    ESP_LOGI(TAG, "Recording storage initialization: %s", esp_err_to_name(err));
    err = InitializeWifi();
    if (err == ESP_OK) {
        CheckWifiRadio();
        const auto saved = SsidManager::GetInstance().GetSsidList();
        if (!saved.empty()) {
            BoardCommand connect{};
            connect.type = 2;
            if (saved.front().ssid.size() <= 32 && saved.front().password.size() <= 64) {
                memcpy(connect.ssid, saved.front().ssid.c_str(), saved.front().ssid.size());
                memcpy(connect.password, saved.front().password.c_str(),
                       saved.front().password.size());
                ConnectWifi(connect, true);
                memset(connect.password, 0, sizeof(connect.password));
            }
        }
    } else {
        std::lock_guard<std::mutex> lock(status_mutex);
        status.wifi = "初始化失败";
    }
    BoardCommand command{};
    LogResources("network-init");
    unsigned idle_ticks = 0;
    while (true) {
        if (xQueueReceive(commands, &command, pdMS_TO_TICKS(5000)) != pdTRUE) {
            LogWifiLink();
            if (++idle_ticks % 6 == 0)
                LogResources("running");
            if (wifi_reconnect && GetBoardPeripheralStatus().wifi_ip == "--") {
                const auto retry = esp_wifi_connect();
                ESP_LOGI(TAG, "Wi-Fi reconnect requested: %s", esp_err_to_name(retry));
            }
            continue;
        }
        if (command.type == 1)
            CheckWifiRadio();
        else if (command.type == 2)
            ConnectWifi(command);
        else if (command.type == 3)
            RecoverSdCard();
        else if (command.type == 4)
            UpdateC6Firmware(command.update_url);
        memset(command.password, 0, sizeof(command.password));
    }
}
}  // namespace

void StartBoardPeripherals(std::function<void(bool)> callback) {
    if (commands)
        return;
    network_callback = std::move(callback);
    commands = xQueueCreate(2, sizeof(BoardCommand));
    if (!commands)
        return;
    if (xTaskCreate(Initialize, "board_init", 6144, nullptr, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create peripheral initialization task");
    }
}

BoardPeripheralStatus GetBoardPeripheralStatus() {
    std::lock_guard<std::mutex> lock(status_mutex);
    return status;
}

bool RequestBoardWifiScan() {
    if (!commands || GetBoardPeripheralStatus().wifi_busy)
        return false;
    BoardCommand command{};
    command.type = 1;
    return xQueueSend(commands, &command, 0) == pdTRUE;
}

bool RequestBoardWifiConnect(const std::string& ssid, const std::string& password) {
    if (!commands || GetBoardPeripheralStatus().wifi_busy || ssid.empty() || ssid.size() > 32 ||
        password.size() > 64 || (!password.empty() && password.size() < 8))
        return false;
    BoardCommand command{};
    command.type = 2;
    memcpy(command.ssid, ssid.data(), ssid.size());
    memcpy(command.password, password.data(), password.size());
    const bool queued = xQueueSend(commands, &command, 0) == pdTRUE;
    memset(command.password, 0, sizeof(command.password));
    return queued;
}

bool RequestBoardStorageCheck() {
    if (!commands)
        return false;
    BoardCommand command{};
    command.type = 3;
    return xQueueSend(commands, &command, 0) == pdTRUE;
}

bool RequestBoardC6Update(const std::string& url) {
    if (!commands || url.empty() || url.size() >= sizeof(BoardCommand::update_url))
        return false;
    BoardCommand command{};
    command.type = 4;
    memcpy(command.update_url, url.data(), url.size());
    return xQueueSend(commands, &command, 0) == pdTRUE;
}

bool IsBoardNetworkConnected() {
    std::lock_guard<std::mutex> lock(status_mutex);
    return network_connected;
}

void StopBoardUsbAudio() {
    std::lock_guard<std::mutex> lock(capture_mutex);
    if (capture_handle && capture_started)
        uac2_host_device_stop(capture_handle);
    capture_started = false;
    capture_bytes = 0;
}

bool ReadBoardUsbAudio(int16_t* samples, int count) {
    // The existing Voice Lab path requests 320 mono samples per 20 ms frame.
    if (!samples || count != 320)
        return false;
    std::unique_lock<std::mutex> lock(capture_mutex);
    if (!capture_handle) {
        lock.unlock();
        vTaskDelay(pdMS_TO_TICKS(20));
        return false;
    }
    if (!capture_started) {
        uac2_host_stream_config_t stream{};
        stream.channels = 2;
        stream.bit_resolution = 16;
        stream.sample_freq = 16000;
        const auto err = uac2_host_device_start(capture_handle, &stream);
        ESP_LOGI(TAG, "Voice Lab USB capture start: %s", esp_err_to_name(err));
        if (err != ESP_OK) {
            lock.unlock();
            vTaskDelay(pdMS_TO_TICKS(100));
            return false;
        }
        capture_started = true;
        capture_bytes = 0;
    }
    const auto deadline = xTaskGetTickCount() + pdMS_TO_TICKS(40);
    while (capture_bytes < sizeof(capture_pcm)) {
        const int32_t remaining = static_cast<int32_t>(deadline - xTaskGetTickCount());
        if (remaining <= 0)
            return false;
        uint32_t received = 0;
        const auto err = uac2_host_device_read(
            capture_handle, reinterpret_cast<uint8_t*>(capture_pcm) + capture_bytes,
            sizeof(capture_pcm) - capture_bytes, &received,
            std::max<uint32_t>(1, remaining * portTICK_PERIOD_MS));
        capture_bytes += received;
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            capture_bytes = 0;
            return false;
        }
    }
    // Preserve a single slot; mixing the two DSP outputs can cancel speech.
    for (int i = 0; i < count; ++i)
        samples[i] = capture_pcm[2 * i];
    capture_bytes = 0;
    return true;
}
