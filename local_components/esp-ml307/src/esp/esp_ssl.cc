#include "esp_ssl.h"
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>

static const char* TAG = "EspSsl";

EspSsl::EspSsl() = default;

EspSsl::~EspSsl() {
    if (InReceiveTask()) {
        ESP_LOGE(TAG, "TLS owner must defer destruction outside its receive callback");
        abort();
    }
    Disconnect();
}

bool EspSsl::Connect(const std::string& host, int port) {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (tls_client_ != nullptr) {
        ESP_LOGE(TAG, "tls client has been initialized");
        return false;
    }

    tls_client_ = esp_tls_init();
    if (tls_client_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize TLS");
        return false;
    }

    esp_tls_cfg_t cfg = {};
    static const int ciphersuites[] = {
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        0,
    };
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.common_name = host.c_str();
    cfg.timeout_ms = 10000;
    cfg.tls_version = ESP_TLS_VER_TLS_1_2;
    cfg.ciphersuites_list = ciphersuites;

    int ret = esp_tls_conn_new_sync(host.c_str(), host.length(), port, &cfg, tls_client_);
    if (ret != 1) {
        esp_tls_error_handle_t last_error;
        if (esp_tls_get_error_handle(tls_client_, &last_error) == ESP_OK) {
            int error_code, error_flags;
            esp_err_t err = esp_tls_get_and_clear_last_error(last_error, &error_code, &error_flags);
            last_error_ = err;
            ESP_LOGE(TAG, "Failed to connect to %s:%d, esp=0x%x, tls=0x%x, flags=0x%x",
                     host.c_str(), port, err, error_code, error_flags);
        } else {
            last_error_ = -1;
            ESP_LOGE(TAG, "Failed to get error handle");
        }
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        return false;
    }

#if CONFIG_VOICE_LAB_STANDALONE_MODE
    // PCM is delivered in small real-time frames. Do not hold a frame behind
    // Nagle's algorithm while waiting for acknowledgement of an earlier write.
    int sockfd = -1;
    const int no_delay = 1;
    if (esp_tls_get_conn_sockfd(tls_client_, &sockfd) != ESP_OK || sockfd < 0 ||
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) != 0) {
        ESP_LOGW(TAG, "Unable to enable TCP_NODELAY: errno=%d", errno);
    } else {
        ESP_LOGI(TAG, "TCP_NODELAY enabled for realtime transport");
    }
    // Bound each blocking socket write so a lost peer cannot indefinitely hold
    // the application/heartbeat sender. The complete TLS send has its own budget.
    const timeval send_timeout = {.tv_sec = 5, .tv_usec = 0};
    if (sockfd >= 0 &&
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) != 0) {
        ESP_LOGW(TAG, "Unable to set socket send timeout: errno=%d", errno);
    }
#endif
    connected_ = true;

    receive_exited_.store(false);
    if (xTaskCreate(
            [](void* arg) {
                EspSsl* ssl = (EspSsl*)arg;
                ssl->ReceiveTask();
                // Last access to ssl. The owner may destroy it as soon as this is true.
                ssl->receive_exited_.store(true, std::memory_order_release);
                vTaskDelete(NULL);
            },
            "ssl_receive", 4096, this, 1, &receive_task_handle_) != pdPASS) {
        connected_ = false;
        receive_exited_.store(true);
        last_error_ = ESP_ERR_NO_MEM;
        std::lock_guard<std::mutex> sender(send_mutex_);
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
        return false;
    }
    return true;
}

bool EspSsl::InReceiveTask() const {
    return !receive_exited_.load(std::memory_order_acquire) &&
           xTaskGetCurrentTaskHandle() == receive_task_handle_;
}

void EspSsl::Disconnect() {
    connected_ = false;
    // A callback can request shutdown, but only an external owner may join and
    // destroy this transport. Never wait for the current receive task itself.
    if (InReceiveTask()) {
        int sockfd = -1;
        if (tls_client_ && esp_tls_get_conn_sockfd(tls_client_, &sockfd) == ESP_OK && sockfd >= 0) {
            shutdown(sockfd, SHUT_RDWR);
        }
        return;
    }
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (tls_client_ != nullptr) {
        int sockfd = -1;
        if (esp_tls_get_conn_sockfd(tls_client_, &sockfd) == ESP_OK && sockfd >= 0) {
            // Wake blocked reads/writes without closing an fd still owned by TLS.
            shutdown(sockfd, SHUT_RDWR);
        }
        const auto started = xTaskGetTickCount();
        while (!receive_exited_.load(std::memory_order_acquire)) {
            if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(10000)) {
                ESP_LOGE(TAG, "TLS receive task did not exit; refusing to free live transport");
                abort();
            }
            vTaskDelay(1);
        }
        receive_task_handle_ = nullptr;
        // Do not hold this lock while joining: the receive task may send a pong.
        std::lock_guard<std::mutex> sender(send_mutex_);
        esp_tls_conn_destroy(tls_client_);
        tls_client_ = nullptr;
    }
}

void EspSsl::FailSend(int error) {
    last_error_ = error;
    connected_ = false;
    int sockfd = -1;
    if (tls_client_ && esp_tls_get_conn_sockfd(tls_client_, &sockfd) == ESP_OK && sockfd >= 0) {
        shutdown(sockfd, SHUT_RDWR);
    }
}

/* CONFIG_MBEDTLS_SSL_RENEGOTIATION should be disabled in sdkconfig.
 * Otherwise, invalid memory access may be triggered.
 */
int EspSsl::Send(const std::string& data) {
#if CONFIG_VOICE_LAB_STANDALONE_MODE
    const auto lock_started_us = esp_timer_get_time();
#endif
    std::lock_guard<std::mutex> sender(send_mutex_);
#if CONFIG_VOICE_LAB_STANDALONE_MODE
    const auto lock_wait_us = esp_timer_get_time() - lock_started_us;
    if (lock_wait_us >= 500000) {
        ESP_LOGW(TAG, "TLS sender lock delayed: wait_ms=%lld bytes=%u",
                 static_cast<long long>(lock_wait_us / 1000), static_cast<unsigned>(data.size()));
    }
#endif
    if (!connected_ || tls_client_ == nullptr) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();
#if CONFIG_VOICE_LAB_STANDALONE_MODE
    const auto send_deadline_us = esp_timer_get_time() + 10000000;
#endif

    while (total_sent < data_size) {
        if (!connected_)
            return -1;
#if CONFIG_VOICE_LAB_STANDALONE_MODE
        if (esp_timer_get_time() >= send_deadline_us) {
            ESP_LOGW(TAG, "TLS send deadline expired: sent=%u bytes=%u",
                     static_cast<unsigned>(total_sent), static_cast<unsigned>(data_size));
            FailSend(ESP_ERR_TIMEOUT);
            return -1;
        }
        const auto write_started_us = esp_timer_get_time();
#endif
        int ret = esp_tls_conn_write(tls_client_, data_ptr + total_sent, data_size - total_sent);
#if CONFIG_VOICE_LAB_STANDALONE_MODE
        const auto write_elapsed_us = esp_timer_get_time() - write_started_us;
        if (write_elapsed_us >= 500000) {
            ESP_LOGW(TAG, "TLS write delayed: write_ms=%lld bytes=%u result=%d errno=%d",
                     static_cast<long long>(write_elapsed_us / 1000),
                     static_cast<unsigned>(data_size - total_sent), ret, ret < 0 ? errno : 0);
        }
#endif

        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE || ret == ESP_TLS_ERR_SSL_WANT_READ) {
            vTaskDelay(1);
            continue;
        }

        if (ret <= 0) {
            ESP_LOGE(TAG, "SSL send failed: ret=%d, errno=%d", ret, errno);
            FailSend(ret < 0 ? ret : ESP_FAIL);
            return -1;
        }

        total_sent += ret;
    }

    return total_sent;
}

void EspSsl::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = esp_tls_conn_read(tls_client_, data.data(), data.size());

        if (ret == ESP_TLS_ERR_SSL_WANT_READ) {
            continue;
        }

        if (ret <= 0) {
            if (ret < 0) {
                ESP_LOGE(TAG, "SSL receive failed: %d", ret);
            }
            connected_ = false;
            // 接收失败或连接断开时调用断连回调
            if (disconnect_callback_) {
                disconnect_callback_();
            }
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
#if CONFIG_VOICE_LAB_STANDALONE_MODE
            const auto callback_started_us = esp_timer_get_time();
#endif
            stream_callback_(data);
#if CONFIG_VOICE_LAB_STANDALONE_MODE
            const auto callback_elapsed_us = esp_timer_get_time() - callback_started_us;
            if (callback_elapsed_us >= 500000) {
                ESP_LOGW(TAG, "TLS receive callback delayed: callback_ms=%lld bytes=%d",
                         static_cast<long long>(callback_elapsed_us / 1000), ret);
            }
#endif
        }
    }
}

int EspSsl::GetLastError() { return last_error_; }
