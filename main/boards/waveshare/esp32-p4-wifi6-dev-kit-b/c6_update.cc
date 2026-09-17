#include "c6_update.h"
#include "voice_lab_client.h"

#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_heap_caps.h>
#include <esp_hosted.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <net/if.h>
#include <psa/crypto.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

// Invoked only by an explicit physical-console maintenance command.
// The URL filename is the SHA-256 chosen by the operator, never by the HTTP server.
void UpdateC6Firmware(const char* url) {
    constexpr char TAG[] = "C6Update";
    if (VoiceLabClient::GetInstance().IsRecording()) {
        ESP_LOGE(TAG, "Stop recording before C6 maintenance");
        return;
    }
    const std::string location(url);
    const auto slash = location.find_last_of('/');
    const auto filename = location.substr(slash == std::string::npos ? 0 : slash + 1);
    if (filename.size() != 68 || filename.substr(64) != ".bin" ||
        filename.substr(0, 64).find_first_not_of("0123456789abcdef") != std::string::npos) {
        ESP_LOGE(TAG, "Use an operator-verified <sha256>.bin URL");
        return;
    }
    auto* ethernet = esp_netif_get_handle_from_ifkey("ETH_DEF");
    struct ifreq interface_name{};
    if (!ethernet || !esp_netif_is_netif_up(ethernet) ||
        esp_netif_get_netif_impl_name(ethernet, interface_name.ifr_name) != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet required for C6 maintenance; C6 unchanged");
        return;
    }
    // Keep Hosted/SDIO running, but remove the competing same-subnet Wi-Fi
    // interface during maintenance. Restore it on every non-reboot exit.
    struct WifiRestore {
        bool stopped = false;
        ~WifiRestore() {
            if (stopped && esp_wifi_start() == ESP_OK)
                esp_wifi_connect();
        }
    } wifi_restore;
    wifi_restore.stopped = esp_wifi_stop() == ESP_OK;
    ESP_LOGI(TAG, "Maintenance Wi-Fi stopped=%d; SDIO retained", wifi_restore.stopped);
    if (!wifi_restore.stopped)
        return;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_http_client_config_t config{};
    config.url = url;
    config.timeout_ms = 10000;
    config.if_name = &interface_name;
    auto cleanup = [](esp_http_client* client) { esp_http_client_cleanup(client); };
    std::unique_ptr<esp_http_client, decltype(cleanup)> http(esp_http_client_init(&config),
                                                             cleanup);
    if (!http)
        return;
    ESP_LOGI(TAG, "Downloading over Ethernet interface %s", interface_name.ifr_name);
    if (esp_http_client_open(http.get(), 0) != ESP_OK) {
        ESP_LOGE(TAG, "Image download failed; C6 unchanged");
        return;
    }
    const auto length = esp_http_client_fetch_headers(http.get());
    if (esp_http_client_get_status_code(http.get()) != 200 || length < 4096 ||
        length > 1920 * 1024) {
        ESP_LOGE(TAG, "Invalid HTTP image response: status=%d length=%lld; C6 unchanged",
                 esp_http_client_get_status_code(http.get()), static_cast<long long>(length));
        return;
    }
    const size_t size = static_cast<size_t>(length);
    std::unique_ptr<uint8_t, decltype(&heap_caps_free)> image(
        static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        heap_caps_free);
    if (!image)
        return;
    size_t offset = 0;
    while (offset < size) {
        const int received =
            esp_http_client_read(http.get(), reinterpret_cast<char*>(image.get() + offset),
                                 std::min<size_t>(4096, size - offset));
        if (received <= 0) {
            ESP_LOGE(TAG, "Incomplete download; C6 unchanged");
            return;
        }
        offset += received;
    }
    esp_http_client_close(http.get());
    uint8_t hash[32];
    size_t hash_size = 0;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, image.get(), size, hash, sizeof(hash), &hash_size) !=
            PSA_SUCCESS ||
        hash_size != sizeof(hash))
        return;
    char digest[65];
    for (size_t i = 0; i < sizeof(hash); ++i)
        snprintf(digest + i * 2, 3, "%02x", hash[i]);
    if (filename.substr(0, 64) != digest) {
        ESP_LOGE(TAG, "SHA-256 mismatch; C6 unchanged");
        return;
    }
    esp_image_header_t header{};
    memcpy(&header, image.get(), sizeof(header));
    esp_app_desc_t description{};
    memcpy(&description, image.get() + sizeof(header) + sizeof(esp_image_segment_header_t),
           sizeof(description));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32C6 ||
        description.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(description.project_name, "network_adapter", sizeof(description.project_name))) {
        ESP_LOGE(TAG, "Not an ESP32-C6 Hosted image; C6 unchanged");
        return;
    }
    if (VoiceLabClient::GetInstance().IsRecording())
        return;
    ESP_LOGI(TAG, "Verified C6 image: version=%.32s bytes=%u sha256=%s", description.version,
             static_cast<unsigned>(size), digest);
    // Remote begin selects the inactive OTA slot. If no slot/API exists, stop here.
    auto err = esp_hosted_slave_ota_begin();
    ESP_LOGI(TAG, "SDIO OTA begin: %s", esp_err_to_name(err));
    if (err != ESP_OK)
        return;
    for (offset = 0; offset < size;) {
        const size_t chunk = std::min<size_t>(1024, size - offset);
        err = esp_hosted_slave_ota_write(image.get() + offset, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SDIO OTA write failed at %u: %s; not activating",
                     static_cast<unsigned>(offset), esp_err_to_name(err));
            return;
        }
        offset += chunk;
        if (offset % 65536 == 0 || offset == size)
            ESP_LOGI(TAG, "SDIO OTA progress %u/%u", static_cast<unsigned>(offset),
                     static_cast<unsigned>(size));
    }
    err = esp_hosted_slave_ota_end();
    ESP_LOGI(TAG, "SDIO OTA end: %s", esp_err_to_name(err));
    if (err != ESP_OK)
        return;
    // New slaves require activate; old slaves activate during OTA end. Verify
    // the actual new version after reboot instead of treating RPC timeout as success.
    err = esp_hosted_slave_ota_activate();
    ESP_LOGI(TAG, "SDIO OTA activate: %s; restarting host to verify", esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}
