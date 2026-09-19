#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Independent disk and HTTP workers. The capture callback only enqueues PCM.
class VoiceLabArchive {
public:
    using Upload = std::function<std::string(const std::string&, const std::string&,
                                             const std::string&, bool)>;
    bool Initialize(const std::string& root, Upload upload);
    bool Begin(const std::string& run, uint64_t boot, uint64_t sample_start);
    bool Push(const std::vector<int16_t>& pcm);
    bool Finish(bool interrupted);
    void RequireManualRecovery() { manual_.store(true); }
    bool Active() const { return active_.load(); }
    bool Failed() const { return failed_.load(); }
    uint64_t Samples() const { return samples_.load(); }
    bool ManualRecovery() const { return manual_.load(); }

private:
    void WriteLoop();
    void UploadLoop();
    bool SaveManifest(bool stopped, bool interrupted);
    bool Seal();
    std::string root_, run_, directory_, boot_;
    uint64_t sample_start_ = 0;
    unsigned sequence_ = 0;
    std::vector<int16_t> segment_;
    QueueHandle_t queue_ = nullptr;
    Upload upload_;
    std::mutex operation_;
    std::atomic<bool> active_{false}, failed_{false}, manual_{false};
    std::atomic<bool> finishing_{false}, finish_interrupted_{false};
    std::atomic<uint64_t> samples_{0};
};
