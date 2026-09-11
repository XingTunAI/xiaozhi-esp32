#ifndef VOICE_LAB_CLIENT_H
#define VOICE_LAB_CLIENT_H

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "protocol.h"
#include "voice_lab_recording_guard.h"
#include "voice_lab_recording_lease.h"
#include "voice_lab_pcm_buffer.h"

#include <web_socket.h>

class VoiceLabClient {
public:
    enum class UserState { Connecting, Ready, Requesting, Starting, Recording, Stopping, Error };
    UserState GetUserState() const { return user_state_.load(); }
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
    // Nonblocking emergency stop; network/application callbacks can call this.
    void AbortRecording();
    void NotifyNetworkDisconnected();
    bool IsConnected() const;
    bool IsRecording() const { return recording_.load(); }

    bool PairWithEnrollmentCode(const std::string& enrollment_code);
    void BeginCustomerBinding();
    std::string GetBindingCode() const;
    void ClearPairing();
    void ResetVoiceLabSettings();
    bool StartRecording(const std::string& recording_id = "",
                        const std::string& mode = "meeting_live");
    bool RequestStartRecording();
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
    static constexpr int kMaxFramesPerPacket = 1;
    // Bound retained audio by duration, independent of variable packet sizes.
    static constexpr uint64_t kMaxUnacknowledgedSamples = kAudioSampleRate * 4;
#if CONFIG_SPIRAM
    // Fixed 384 KB PSRAM budget: tolerate observed multi-second transport stalls
    // without persisting recordings or delaying the normal immediate send path.
    static constexpr int kMaxPendingPcmFrames = 12000 / kAudioFrameDurationMs;
#else
    static constexpr int kMaxPendingPcmFrames = 1000 / kAudioFrameDurationMs;
#endif

    VoiceLabClient() = default;
    ~VoiceLabClient();

    mutable std::mutex mutex_;
    mutable std::mutex binding_mutex_;
    std::atomic<bool> binding_active_{false};
    std::atomic<bool> customer_bound_{false};
    std::string binding_code_;
    int64_t binding_code_deadline_us_ = 0;
    std::mutex recording_mutex_;
    std::mutex pcm_mutex_;
    // Short metadata operations only; never held across network/audio waits.
    mutable std::mutex authorization_mutex_;
    VoiceLabRecordingLease recording_lease_;
    std::string pending_request_id_;
    std::string active_session_id_;
    std::atomic<int64_t> lease_deadline_us_{0};
    std::atomic<bool> session_stop_requested_{false};
    uint32_t recording_control_epoch_ = 0;  // Protected by recording_mutex_.
    // Original start identity, retained through local or server-requested stop.
    std::string recording_id_;     // Protected by recording_mutex_.
    int recording_revision_ = -1;  // Protected by recording_mutex_.
    // Only guards quick capture gate changes; never held across waits or I/O.
    std::mutex capture_gate_mutex_;
    std::atomic<int> latest_idle_revision_{-1};
    std::atomic<int> active_revision_{-1};
    std::atomic<UserState> user_state_{UserState::Connecting};
    std::atomic<bool> interrupted_{false};
    std::atomic<bool> audio_accepted_{false};
    std::atomic<bool> first_pcm_received_{false};
    std::atomic<bool> audio_completed_{false};
    std::atomic<bool> tail_pending_{false};
    std::atomic<bool> capture_stop_failed_{false};
    std::atomic<int64_t> recording_deadline_us_{0};
    std::atomic<int64_t> start_request_deadline_us_{0};
    std::atomic<int64_t> last_control_rx_us_{0};
    std::atomic<uint64_t> acknowledged_sample_end_{0};
    std::atomic<uint64_t> sent_sample_end_{0};
    std::atomic<uint32_t> control_epoch_{0};
    std::atomic<uint32_t> connection_generation_{0};
    std::atomic<uint32_t> abort_generation_{0};
    uint32_t handled_control_epoch_ = 0;
    VoiceLabRecordingGuard recording_guard_;
    QueueHandle_t control_queue_ = nullptr;
    esp_timer_handle_t safety_timer_ = nullptr;
    struct ControlMessage {
        cJSON* json;
        uint32_t epoch;
        int64_t received_at_us;
    };
    std::unique_ptr<WebSocket> control_websocket_;
    std::unique_ptr<WebSocket> audio_websocket_;
    std::atomic<bool> connecting_{false};
    std::atomic<bool> reconnect_scheduled_{false};
    std::atomic<bool> heartbeat_started_{false};
    std::atomic<bool> manual_disconnect_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> audio_connected_{false};
    std::atomic<bool> recording_{false};
    std::atomic<uint32_t> capture_generation_{0};
    std::string recording_mode_ = "idle";
    uint64_t boot_id_ = 0;
    uint64_t audio_sequence_ = 0;
    uint64_t sample_start_ = 0;
    uint64_t audio_frames_sent_ = 0;
    uint64_t audio_bytes_sent_ = 0;
    uint64_t last_audio_stats_us_ = 0;
    VoiceLabPcmBuffer pending_audio_pcm_;
    struct PendingPacket {
        std::string bytes;
        uint64_t sample_end;
        int64_t last_sent_us;
        unsigned retries;
    };
    std::deque<PendingPacket> unacknowledged_audio_;

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
    bool RunCustomerBinding(const std::string& existing_token);
    bool ConfirmPendingBinding();
    cJSON* BindingHttp(const char* action, const std::string& proof,
                       const std::string& existing_token, int& status);
    bool ConnectControlLocked(const std::string& url, const std::string& token);
    bool ConnectAudioLocked(const std::string& url, const std::string& token);
    void CloseAudioLocked();
    void EnsureHeartbeatTask();
    bool EnsureControlTask();
    bool StartAuthorizedRecording(const std::string& recording_id, const std::string& mode,
                                  int revision, uint32_t authorized_epoch,
                                  const cJSON* authorization = nullptr, int64_t received_at_us = 0,
                                  const cJSON* capture_plan = nullptr);
    bool ApplyRecordingAuthorization(const cJSON* authorization, int64_t received_at_us);
    void HandleRecordingResponse(const cJSON* root);
    void MaybeRenewRecordingLease();
    void AppendRecordingIdentity(cJSON* root) const;
    void AppendCaptureIdentity(cJSON* root) const;  // Caller holds recording_mutex_.
    uint64_t CaptureSampleEnd();                    // Caller has completed the input stop barrier.
    void SetUserState(UserState state);
    bool RetransmitAudioLocked();
    void EnsureUsbProvisioningTask();
    void ScheduleReconnect();
    void SendHelloLocked();
    void SendConfigAck(uint32_t authorized_epoch, int revision, const std::string& mode,
                       const cJSON* capture_plan, bool success, const std::string& error = "");
    void ApplyDesiredConfig(const cJSON* root, uint32_t authorized_epoch, int64_t received_at_us);
    static std::string PrintJson(cJSON* root);
    bool SendJson(const std::string& json);
    bool SendJson(const std::string& json, uint32_t expected_epoch);
    bool SendAudioJson(const std::string& json);
    void SetRecordingIndicator(bool recording);
    void NotifyRecordingStarted();
    bool FlushPendingAudioLocked();
    std::string BuildPcmPacket(const std::vector<int16_t>& pcm, uint64_t first_sequence,
                               uint64_t first_sample_start) const;
    void HandleJson(const cJSON* root);
    void HandleAudioJson(const cJSON* root);
};

#endif
