#ifndef VOICE_LAB_CLIENT_H
#define VOICE_LAB_CLIENT_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "protocol.h"

#include <web_socket.h>

class VoiceLabClient {
public:
    static VoiceLabClient& GetInstance() {
        static VoiceLabClient instance;
        return instance;
    }

    VoiceLabClient(const VoiceLabClient&) = delete;
    VoiceLabClient& operator=(const VoiceLabClient&) = delete;

    void RegisterMcpTools();
    void ConnectAsync();
    bool Connect();
    void Disconnect();
    bool IsConnected() const;
    bool IsRecording() const { return recording_.load(); }

    bool PairWithEnrollmentCode(const std::string& enrollment_code);
    void ClearPairing();
    void ResetVoiceLabSettings();
    bool StartRecording(const std::string& recording_id = "", const std::string& mode = "meeting_live");
    bool StopRecording();
    bool RequestPlayback(const std::string& url);
    void StopPlayback();
    bool SendPcmAudio(std::vector<int16_t>&& pcm);
    std::string GetStatusJson() const;

private:
    static constexpr int kAudioSampleRate = 16000;
    static constexpr int kAudioChannels = 1;
    static constexpr int kAudioFrameDurationMs = 20;
    static constexpr int kPcmSamplesPerFrame = kAudioSampleRate * kAudioFrameDurationMs / 1000;
    static constexpr int kMaxFramesPerPacket = 25;

    VoiceLabClient() = default;
    ~VoiceLabClient();

    mutable std::mutex mutex_;
    std::unique_ptr<WebSocket> control_websocket_;
    std::unique_ptr<WebSocket> audio_websocket_;
    std::atomic<bool> connecting_{false};
    std::atomic<bool> reconnect_scheduled_{false};
    std::atomic<bool> heartbeat_started_{false};
    std::atomic<bool> manual_disconnect_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> audio_connected_{false};
    std::atomic<bool> recording_{false};
    std::string recording_id_;
    std::string recording_mode_ = "idle";
    uint64_t boot_id_ = 0;
    uint64_t audio_sequence_ = 0;
    uint64_t sample_start_ = 0;
    uint64_t audio_frames_sent_ = 0;
    uint64_t audio_bytes_sent_ = 0;
    uint64_t last_audio_stats_us_ = 0;
    std::vector<int16_t> pending_audio_pcm_;

    bool IsAutoConnectEnabled() const;
    std::string GetConfiguredHost() const;
    int GetConfiguredPort() const;
    bool IsTlsEnabled() const;
    std::string GetConfiguredServerUrl() const;
    std::string GetDeviceKey() const;
    std::string GetDeviceToken() const;
    std::string GetEnrollmentCode() const;
    std::string GetHardwareProfile() const;
    std::string GetAudioFrontend() const;
    std::string BuildHttpUrl(const std::string& path) const;
    std::string BuildWebsocketUrl(const std::string& path) const;
    bool EnrollIfNeeded();
    bool ConnectControlLocked(const std::string& url, const std::string& token);
    bool ConnectAudioLocked(const std::string& url, const std::string& token);
    void CloseAudioLocked();
    void EnsureHeartbeatTask();
    void EnsureUsbProvisioningTask();
    void ScheduleReconnect();
    void SendHelloLocked();
    void SendConfigAck(int revision, const std::string& mode, const cJSON* capture_plan, bool success, const std::string& error = "");
    void ApplyDesiredConfig(const cJSON* root);
    static std::string PrintJson(cJSON* root);
    bool SendJson(const std::string& json);
    bool SendAudioJson(const std::string& json);
    void SetRecordingIndicator(bool recording);
    void NotifyRecordingStarted();
    bool FlushPendingAudioLocked(bool force);
    std::string BuildPcmPacket(const std::vector<int16_t>& pcm,
                               uint64_t first_sequence,
                               uint64_t first_sample_start) const;
    void HandleJson(const cJSON* root);
    void HandleAudioJson(const cJSON* root);
};

#endif
