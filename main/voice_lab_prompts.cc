#include "voice_lab_prompts.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <wifi_manager.h>
#include <mutex>
#include "application.h"
#include "assets/lang_config.h"
#include "voice_lab_client.h"

namespace {
std::mutex prompt_mutex;

bool DrainPrompt(AudioService& audio) {
    const auto deadline = esp_timer_get_time() + 8000000;
    while (!audio.IsPlaybackIdle() && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return audio.IsPlaybackIdle();
}
}  // namespace

bool SpeakVoiceLabPrompt(VoiceLabPrompt prompt) {
#if CONFIG_VOICE_LAB_STANDALONE_MODE
    std::lock_guard<std::mutex> lock(prompt_mutex);
    auto& client = VoiceLabClient::GetInstance();
    if (client.IsRecording())
        return false;
    if (prompt == VoiceLabPrompt::StartRequested &&
        client.GetUserState() != VoiceLabClient::UserState::Requesting)
        return false;
    if (prompt == VoiceLabPrompt::RecordingStarted &&
        client.GetUserState() != VoiceLabClient::UserState::Starting)
        return false;
    if (client.GetUserState() == VoiceLabClient::UserState::Starting &&
        prompt != VoiceLabPrompt::RecordingStarted)
        return false;
    auto& audio = Application::GetInstance().GetAudioService();
    if (!DrainPrompt(audio))
        return false;
    std::string_view sound;
    switch (prompt) {
        case VoiceLabPrompt::WifiSetup: {
            auto& wifi = WifiManager::GetInstance();
            if (!wifi.IsConfigMode())
                return false;
            // Physical playback only. Never use Alert, status JSON or logs.
            const auto password = wifi.GetApPassword();
            if (password.empty()) {
                audio.PlaySound(Lang::Sounds::OGG_VL_WIFI_OPEN);
                return DrainPrompt(audio);
            }
            if (password.size() != 8)
                return false;
            audio.PlaySound(Lang::Sounds::OGG_VL_WIFI_SETUP);
            const std::string_view digits[] = {
                Lang::Sounds::OGG_0, Lang::Sounds::OGG_1, Lang::Sounds::OGG_2, Lang::Sounds::OGG_3,
                Lang::Sounds::OGG_4, Lang::Sounds::OGG_5, Lang::Sounds::OGG_6, Lang::Sounds::OGG_7,
                Lang::Sounds::OGG_8, Lang::Sounds::OGG_9};
            for (char digit : password) {
                if (digit < '0' || digit > '9' || !wifi.IsConfigMode())
                    return false;
                audio.PlaySound(digits[digit - '0']);
            }
            audio.PlaySound(Lang::Sounds::OGG_VL_WIFI_FINISH);
            return DrainPrompt(audio);
        }
        case VoiceLabPrompt::Connected:
            sound = Lang::Sounds::OGG_VL_CONNECTED;
            break;
        case VoiceLabPrompt::BindingCode: {
            const auto code = client.GetBindingCode();
            if (code.size() != 8)
                return false;
            audio.PlaySound(Lang::Sounds::OGG_VL_BINDING_CODE);
            const std::string_view digits[] = {
                Lang::Sounds::OGG_0, Lang::Sounds::OGG_1, Lang::Sounds::OGG_2, Lang::Sounds::OGG_3,
                Lang::Sounds::OGG_4, Lang::Sounds::OGG_5, Lang::Sounds::OGG_6, Lang::Sounds::OGG_7,
                Lang::Sounds::OGG_8, Lang::Sounds::OGG_9};
            for (char digit : code) {
                if (digit < '0' || digit > '9')
                    return false;
                audio.PlaySound(digits[digit - '0']);
            }
            return DrainPrompt(audio);
        }
        case VoiceLabPrompt::BindingDone:
            sound = Lang::Sounds::OGG_VL_BINDING_DONE;
            break;
        case VoiceLabPrompt::RecordingStarted:
            sound = Lang::Sounds::OGG_VL_RECORDING_START;
            break;
        case VoiceLabPrompt::RecordingStopped:
            sound = Lang::Sounds::OGG_VL_RECORDING_STOP;
            break;
        case VoiceLabPrompt::StartRequested:
            sound = Lang::Sounds::OGG_VL_START_REQUESTED;
            break;
        case VoiceLabPrompt::StartFailed:
            sound = Lang::Sounds::OGG_VL_START_FAILED;
            break;
        case VoiceLabPrompt::Interrupted:
            sound = Lang::Sounds::OGG_VL_INTERRUPTED;
            break;
    }
    audio.PlaySound(sound);
    return DrainPrompt(audio);
#else
    return false;
#endif
}

void QueueVoiceLabPrompt(VoiceLabPrompt prompt) {
#if CONFIG_VOICE_LAB_STANDALONE_MODE
    static QueueHandle_t queue = []() {
        auto handle = xQueueCreate(4, sizeof(VoiceLabPrompt));
        if (!handle)
            return static_cast<QueueHandle_t>(nullptr);
        if (xTaskCreate(
                [](void* arg) {
                    auto handle = static_cast<QueueHandle_t>(arg);
                    VoiceLabPrompt next;
                    while (true) {
                        if (xQueueReceive(handle, &next, portMAX_DELAY) == pdTRUE)
                            SpeakVoiceLabPrompt(next);
                    }
                },
                "vl_prompts", 4096, handle, 2, nullptr) != pdPASS) {
            vQueueDelete(handle);
            return static_cast<QueueHandle_t>(nullptr);
        }
        return handle;
    }();
    if (queue)
        xQueueSend(queue, &prompt, 0);
#endif
}
