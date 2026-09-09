#ifndef VOICE_LAB_RECORDING_GUARD_H
#define VOICE_LAB_RECORDING_GUARD_H

// A new control connection first synchronizes an idle state. A saved active
// configuration is never permission to resume capture after reboot/reconnect.
// This class is used only by the serialized control worker.
class VoiceLabRecordingGuard {
public:
    enum class Decision { Start, Stop, Duplicate, NeedIdle, Stale, Busy };

    static constexpr bool StartSuperseded(int start_revision, int received_idle_revision) {
        return start_revision >= 0 && received_idle_revision > start_revision;
    }

    constexpr void Reset() {
        synchronized_ = false;
        revision_ = -1;
        active_ = false;
    }

    constexpr Decision Apply(int revision, bool active, bool recording) {
        if (revision < revision_)
            return Decision::Stale;
        if (revision == revision_) {
            if (active == active_ && (!active || recording))
                return Decision::Duplicate;
            return Decision::Stale;
        }
        if (!synchronized_ && active) {
            revision_ = revision;
            active_ = active;
            return Decision::NeedIdle;
        }
        if (active && recording)
            return Decision::Busy;
        revision_ = revision;
        active_ = active;
        if (!active) {
            synchronized_ = true;
            return Decision::Stop;
        }
        return Decision::Start;
    }

private:
    bool synchronized_ = false;
    int revision_ = -1;
    bool active_ = false;
};

#endif
