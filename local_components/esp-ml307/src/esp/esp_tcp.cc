#include "esp_tcp.h"

#include <esp_log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>

static const char* TAG = "EspTcp";

EspTcp::EspTcp() = default;

EspTcp::~EspTcp() {
    if (InReceiveTask()) {
        ESP_LOGE(TAG, "TCP owner must defer destruction outside its receive callback");
        abort();
    }
    Disconnect();
}

bool EspTcp::Connect(const std::string& host, int port) {
    // Reap a previous receive task even after the peer has disconnected.
    Disconnect();
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);

    struct sockaddr_in server_addr;
    bzero(&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    // host is domain
    struct hostent* server = gethostbyname(host.c_str());
    if (server == NULL) {
        last_error_ = h_errno;
        ESP_LOGE(TAG, "Failed to get host by name");
        return false;
    }
    memcpy(&server_addr.sin_addr, server->h_addr, server->h_length);
    ESP_LOGI(TAG, "Resolved %s -> %s", host.c_str(), inet_ntoa(*(struct in_addr*)server->h_addr));

    tcp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd_ < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to create socket");
        return false;
    }

    int ret = connect(tcp_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        last_error_ = errno;
        ESP_LOGE(TAG, "Failed to connect to %s:%d, code=0x%x", host.c_str(), port, last_error_);
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }

    connected_ = true;

    receive_exited_.store(false);
    if (xTaskCreate(
            [](void* arg) {
                EspTcp* tcp = (EspTcp*)arg;
                tcp->ReceiveTask();
                // Last access to tcp; no event-group waiter can free it before this point.
                tcp->receive_exited_.store(true, std::memory_order_release);
                vTaskDelete(NULL);
            },
            "tcp_receive", 4096, this, 1, &receive_task_handle_) != pdPASS) {
        connected_ = false;
        receive_exited_.store(true);
        last_error_ = ENOMEM;
        std::lock_guard<std::mutex> sender(send_mutex_);
        close(tcp_fd_);
        tcp_fd_ = -1;
        return false;
    }
    return true;
}

bool EspTcp::InReceiveTask() const {
    return !receive_exited_.load(std::memory_order_acquire) &&
           xTaskGetCurrentTaskHandle() == receive_task_handle_;
}

void EspTcp::Disconnect() { DoDisconnect(!InReceiveTask()); }

void EspTcp::DoDisconnect(bool wait_for_task) {
    const bool notify = connected_.exchange(false);
    if (wait_for_task) {
        std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
        if (tcp_fd_ >= 0) {
            shutdown(tcp_fd_, SHUT_RDWR);
        }
        const auto started = xTaskGetTickCount();
        while (!receive_exited_.load(std::memory_order_acquire)) {
            if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(10000)) {
                ESP_LOGE(TAG, "TCP receive task did not exit; refusing to free live transport");
                abort();
            }
            vTaskDelay(1);
        }
        receive_task_handle_ = nullptr;
        std::lock_guard<std::mutex> sender(send_mutex_);
        if (tcp_fd_ >= 0) {
            close(tcp_fd_);
            tcp_fd_ = -1;
        }
    } else if (tcp_fd_ >= 0) {
        // Receive callbacks may stop I/O, but the external owner closes the fd
        // only after both the receive task and any concurrent sender finish.
        shutdown(tcp_fd_, SHUT_RDWR);
    }
    if (notify && disconnect_callback_) {
        disconnect_callback_();
    }
}

int EspTcp::Send(const std::string& data) {
    std::lock_guard<std::mutex> sender(send_mutex_);
    if (!connected_ || tcp_fd_ < 0) {
        ESP_LOGE(TAG, "Not connected");
        return -1;
    }

    size_t total_sent = 0;
    size_t data_size = data.size();
    const char* data_ptr = data.data();

    while (total_sent < data_size) {
        if (!connected_)
            return -1;
        int ret = send(tcp_fd_, data_ptr + total_sent, data_size - total_sent, 0);

        if (ret <= 0) {
            ESP_LOGE(TAG, "Send failed: ret=%d, errno=%d", ret, errno);
            return ret;
        }

        total_sent += ret;
    }

    return total_sent;
}

void EspTcp::ReceiveTask() {
    std::string data;
    while (connected_) {
        data.resize(1500);
        int ret = recv(tcp_fd_, data.data(), data.size(), 0);
        if (ret <= 0) {
            if (ret < 0) {
                ESP_LOGE(TAG, "TCP receive failed: %d", ret);
            }
            // 被动断开，不需要等待接收任务退出（当前就是接收任务）
            DoDisconnect(false);
            break;
        }

        if (stream_callback_) {
            data.resize(ret);
            stream_callback_(data);
        }
    }
}

int EspTcp::GetLastError() { return last_error_; }
