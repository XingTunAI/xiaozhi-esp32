#ifndef _ESP_TCP_H_
#define _ESP_TCP_H_

#include <atomic>
#include <mutex>
#include "tcp.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class EspTcp : public Tcp {
public:
    EspTcp();
    ~EspTcp();

    bool Connect(const std::string& host, int port) override;
    void Disconnect() override;
    int Send(const std::string& data) override;

    int GetLastError() override;

private:
    int tcp_fd_ = -1;
    TaskHandle_t receive_task_handle_ = nullptr;
    std::atomic<bool> receive_exited_{true};
    std::mutex lifecycle_mutex_;
    std::mutex send_mutex_;
    int last_error_ = 0;

    void ReceiveTask();
    bool InReceiveTask() const;
    // 内部断开处理函数
    // wait_for_task: 是否等待接收任务退出（主动断开为true，被动断开为false）
    void DoDisconnect(bool wait_for_task);
};

#endif  // _ESP_TCP_H_
