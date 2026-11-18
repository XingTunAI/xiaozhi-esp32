#include "net_diag.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/priv/tcp_priv.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>
#include <ping/ping_sock.h>

namespace {
constexpr char TAG[] = "P4NetDiag";
std::atomic<bool> watch_enabled{false};
std::atomic<bool> watch_task_started{false};
std::atomic<bool> tput_running{false};

struct PcbSample {
    uint16_t local_port, remote_port;
    uint8_t state, nrtx, dupacks;
    uint16_t flags, snd_queuelen, mss;
    int16_t rto, sa, sv;
    uint32_t cwnd, ssthresh, snd_wnd, snd_buf, in_flight, buffered;
};

struct SampleRequest {
    SemaphoreHandle_t done;
    uint16_t remote_port;  // 0 = all
    PcbSample samples[8];
    int count;
};

// Runs in the tcpip thread: lwIP PCBs must not be walked from another task.
void CollectPcbs(void* arg) {
    auto* request = static_cast<SampleRequest*>(arg);
    request->count = 0;
    for (tcp_pcb* pcb = tcp_active_pcbs; pcb && request->count < 8; pcb = pcb->next) {
        if (request->remote_port && pcb->remote_port != request->remote_port)
            continue;
        auto& s = request->samples[request->count++];
        s.local_port = pcb->local_port;
        s.remote_port = pcb->remote_port;
        s.state = pcb->state;
        s.nrtx = pcb->nrtx;
        s.dupacks = pcb->dupacks;
        s.flags = pcb->flags;
        s.snd_queuelen = pcb->snd_queuelen;
        s.mss = pcb->mss;
        s.rto = pcb->rto;
        s.sa = pcb->sa;
        s.sv = pcb->sv;
        s.cwnd = pcb->cwnd;
        s.ssthresh = pcb->ssthresh;
        s.snd_wnd = pcb->snd_wnd;
        s.snd_buf = pcb->snd_buf;
        s.in_flight = pcb->snd_nxt - pcb->lastack;
        s.buffered = pcb->snd_lbb - pcb->lastack;
    }
    xSemaphoreGive(request->done);
}

void LogPcbs(uint16_t remote_port, const char* label) {
    auto request = std::make_unique<SampleRequest>();
    request->done = xSemaphoreCreateBinary();
    request->remote_port = remote_port;
    if (!request->done)
        return;
    if (tcpip_callback(CollectPcbs, request.get()) != ERR_OK ||
        xSemaphoreTake(request->done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        // The callback may still run later; leak rather than free memory it will touch.
        ESP_LOGW(TAG, "TCP sample timed out");
        request.release();
        return;
    }
    for (int i = 0; i < request->count; ++i) {
        const auto& s = request->samples[i];
        // rto/srtt are lwIP slow-timer ticks (TCP_SLOW_INTERVAL ms each).
        ESP_LOGI(TAG,
                 "%s tcp lport=%u rport=%u st=%u cwnd=%lu ssth=%lu wnd=%lu inflight=%lu "
                 "buffered=%lu snd_buf=%lu qlen=%u rto_ms=%d srtt_ms=%d nrtx=%u dup=%u "
                 "infr=%d rto_flag=%d",
                 label, s.local_port, s.remote_port, s.state, (unsigned long)s.cwnd,
                 (unsigned long)s.ssthresh, (unsigned long)s.snd_wnd, (unsigned long)s.in_flight,
                 (unsigned long)s.buffered, (unsigned long)s.snd_buf, s.snd_queuelen,
                 s.rto * TCP_SLOW_INTERVAL, (s.sa >> 3) * TCP_SLOW_INTERVAL, s.nrtx, s.dupacks,
                 (s.flags & TF_INFR) != 0, (s.flags & TF_RTO) != 0);
    }
    vSemaphoreDelete(request->done);
}

void EnsureWatchTask() {
    bool expected = false;
    if (!watch_task_started.compare_exchange_strong(expected, true))
        return;
    xTaskCreate(
        [](void*) {
            while (true) {
                if (watch_enabled.load())
                    LogPcbs(443, "watch");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        },
        "p4_net_watch", 4096, nullptr, 1, nullptr);
}

void StartPing(const std::string& host, int count) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) {
        ESP_LOGW(TAG, "ping: cannot resolve %s", host.c_str());
        return;
    }
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    ip4_addr_t target;
    inet_addr_to_ip4addr(&target, &reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr);
    freeaddrinfo(result);
    ip_addr_copy_from_ip4(config.target_addr, target);
    config.count = count;
    config.interval_ms = 200;
    config.timeout_ms = 2000;
    esp_ping_callbacks_t callbacks{};
    callbacks.on_ping_success = [](esp_ping_handle_t handle, void*) {
        uint32_t elapsed = 0;
        uint16_t seq = 0;
        esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
        esp_ping_get_profile(handle, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
        ESP_LOGI(TAG, "ping seq=%u rtt_ms=%lu", seq, (unsigned long)elapsed);
    };
    callbacks.on_ping_timeout = [](esp_ping_handle_t handle, void*) {
        uint16_t seq = 0;
        esp_ping_get_profile(handle, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
        ESP_LOGW(TAG, "ping seq=%u timeout", seq);
    };
    callbacks.on_ping_end = [](esp_ping_handle_t handle, void*) {
        uint32_t sent = 0, received = 0, total_ms = 0;
        esp_ping_get_profile(handle, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
        esp_ping_get_profile(handle, ESP_PING_PROF_REPLY, &received, sizeof(received));
        esp_ping_get_profile(handle, ESP_PING_PROF_DURATION, &total_ms, sizeof(total_ms));
        ESP_LOGI(TAG, "ping summary: sent=%lu received=%lu", (unsigned long)sent,
                 (unsigned long)received);
        esp_ping_delete_session(handle);
    };
    esp_ping_handle_t handle = nullptr;
    if (esp_ping_new_session(&config, &callbacks, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "ping: session failed");
        return;
    }
    esp_ping_start(handle);
}

struct TputArgs {
    sockaddr_in peer;
    int seconds;
};

void TputTask(void* arg) {
    std::unique_ptr<TputArgs> args(static_cast<TputArgs*>(arg));
    constexpr size_t kChunk = 16384;
    std::vector<char> buffer(kChunk, 0x5a);
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&args->peer), sizeof(args->peer)) != 0) {
        ESP_LOGW(TAG, "tput: connect failed errno=%d", errno);
        if (fd >= 0)
            close(fd);
        tput_running.store(false);
        vTaskDelete(nullptr);
        return;
    }
    const int no_delay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
    ESP_LOGI(TAG, "tput: connected, sending %d s in %u-byte writes (tcp_snd_buf=%d)", args->seconds,
             (unsigned)kChunk, CONFIG_LWIP_TCP_SND_BUF_DEFAULT);
    const int64_t start = esp_timer_get_time();
    const int64_t end = start + static_cast<int64_t>(args->seconds) * 1000000;
    int64_t second_start = start, max_write_us = 0;
    uint64_t total = 0, second_bytes = 0;
    while (esp_timer_get_time() < end) {
        const int64_t before = esp_timer_get_time();
        const int written = send(fd, buffer.data(), buffer.size(), 0);
        const int64_t after = esp_timer_get_time();
        if (written <= 0) {
            ESP_LOGW(TAG, "tput: send failed errno=%d after %llu bytes", errno,
                     (unsigned long long)total);
            break;
        }
        max_write_us = std::max(max_write_us, after - before);
        total += written;
        second_bytes += written;
        if (after - second_start >= 1000000) {
            ESP_LOGI(TAG, "tput: %llu kbit/s max_write_ms=%lld",
                     (unsigned long long)(second_bytes * 8 * 1000 / (after - second_start)),
                     (long long)(max_write_us / 1000));
            LogPcbs(ntohs(args->peer.sin_port), "tput");
            second_start = after;
            second_bytes = 0;
            max_write_us = 0;
        }
    }
    const int64_t elapsed = esp_timer_get_time() - start;
    ESP_LOGI(TAG, "tput summary: bytes=%llu seconds=%.1f avg_kbit_s=%llu",
             (unsigned long long)total, elapsed / 1e6,
             (unsigned long long)(elapsed > 0 ? total * 8 * 1000 / elapsed : 0));
    shutdown(fd, SHUT_RDWR);
    close(fd);
    tput_running.store(false);
    vTaskDelete(nullptr);
}

}  // namespace

bool HandleBoardNetDiagCommand(const std::string& command) {
    const std::string prefix = "p4 net ";
    if (command.compare(0, prefix.size(), prefix) != 0)
        return false;
    const auto rest = command.substr(prefix.size());
    std::vector<std::string> words;
    for (size_t i = 0; i < rest.size();) {
        const auto j = rest.find(' ', i);
        const auto word = rest.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!word.empty())
            words.push_back(word);
        if (j == std::string::npos)
            break;
        i = j + 1;
    }
    if (words.size() >= 2 && words[0] == "ping") {
        StartPing(words[1], words.size() >= 3 ? std::max(1, atoi(words[2].c_str())) : 10);
    } else if (words.size() == 1 && words[0] == "tcp") {
        LogPcbs(0, "sample");
    } else if (words.size() == 2 && words[0] == "watch") {
        EnsureWatchTask();
        watch_enabled.store(words[1] == "on");
        ESP_LOGI(TAG, "TCP watch %s", watch_enabled.load() ? "on" : "off");
    } else if (words.size() == 4 && words[0] == "tput") {
        bool expected = false;
        if (!tput_running.compare_exchange_strong(expected, true)) {
            ESP_LOGW(TAG, "tput already running");
            return true;
        }
        auto* args = new TputArgs{};
        args->peer.sin_family = AF_INET;
        args->peer.sin_port = htons(static_cast<uint16_t>(atoi(words[2].c_str())));
        args->seconds = std::max(1, std::min(120, atoi(words[3].c_str())));
        if (inet_pton(AF_INET, words[1].c_str(), &args->peer.sin_addr) != 1 ||
            xTaskCreate(TputTask, "p4_net_tput", 6144, args, 2, nullptr) != pdPASS) {
            delete args;
            tput_running.store(false);
            ESP_LOGW(TAG, "tput: bad address or task failed");
        }
    } else {
        ESP_LOGI(TAG,
                 "usage: p4 net ping <host> [count] | tcp | watch <on|off> | "
                 "tput <ipv4> <port> <seconds>");
    }
    return true;
}
