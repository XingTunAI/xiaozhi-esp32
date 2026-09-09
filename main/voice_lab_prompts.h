#ifndef VOICE_LAB_PROMPTS_H
#define VOICE_LAB_PROMPTS_H

enum class VoiceLabPrompt {
    WifiSetup,
    Connected,
    RecordingStarted,
    RecordingStopped,
    StartRequested,
    StartFailed,
    Interrupted
};

// Blocking playback/drain is for worker tasks only. Network/main callbacks use
// the bounded async queue. Prompts never log or upload the hotspot password.
bool SpeakVoiceLabPrompt(VoiceLabPrompt prompt);
void QueueVoiceLabPrompt(VoiceLabPrompt prompt);

#endif
