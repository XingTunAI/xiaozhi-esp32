#ifndef _ESP_SSL_H_
#define _ESP_SSL_H_

#include <esp_tls.h>
#include <atomic>
#include <mutex>
#include "tcp.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class EspSsl : public Tcp {
public:
    EspSsl();
    ~EspSsl();

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;

    int GetLastError() override;

private:
    esp_tls_t* tls_client_ = nullptr;
    TaskHandle_t receive_task_handle_ = nullptr;
    std::atomic<bool> receive_exited_{true};
    std::mutex lifecycle_mutex_;
    std::mutex send_mutex_;
    int last_error_ = 0;

    void ReceiveTask();
    bool InReceiveTask() const;
};

#endif  // _ESP_SSL_H_
