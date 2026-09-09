#include "voice_lab_recording_guard.h"

constexpr int run_recording_guard_test(int scenario) {
    VoiceLabRecordingGuard guard;
    using D = VoiceLabRecordingGuard::Decision;
#define CHECK(expression) \
    if (!(expression))    \
    return __LINE__
    switch (scenario) {
        case 0:  // Boot/reconnect cannot restore an active snapshot.
            CHECK(guard.Apply(10, true, false) == D::NeedIdle);
            CHECK(guard.Apply(10, true, false) == D::Stale);
            CHECK(guard.Apply(11, false, false) == D::Stop);
            CHECK(guard.Apply(12, true, false) == D::Start);
            guard.Reset();
            CHECK(guard.Apply(12, true, false) == D::NeedIdle);
            break;
        case 1:  // Duplicate active commands never restart or reset audio.
            CHECK(guard.Apply(0, false, false) == D::Stop);
            CHECK(guard.Apply(1, true, false) == D::Start);
            CHECK(guard.Apply(1, true, true) == D::Duplicate);
            CHECK(guard.Apply(1, false, true) == D::Stale);
            CHECK(guard.Apply(0, false, true) == D::Stale);
            break;
        case 2:  // A different test cannot replace an existing recording.
            CHECK(guard.Apply(1, false, false) == D::Stop);
            CHECK(guard.Apply(2, true, false) == D::Start);
            CHECK(guard.Apply(3, true, true) == D::Busy);
            CHECK(guard.Apply(3, false, true) == D::Stop);
            break;
        case 3:  // A local stop cannot be undone by replaying its start.
            CHECK(guard.Apply(1, false, false) == D::Stop);
            CHECK(guard.Apply(2, true, false) == D::Start);
            CHECK(guard.Apply(2, true, false) == D::Stale);
            CHECK(guard.Apply(3, false, false) == D::Stop);
            CHECK(guard.Apply(4, true, false) == D::Start);
            break;
        case 4:  // Repeated idle and long-lived revision values are valid.
            CHECK(guard.Apply(2147483645, false, false) == D::Stop);
            CHECK(guard.Apply(2147483645, false, false) == D::Duplicate);
            CHECK(guard.Apply(2147483646, true, false) == D::Start);
            CHECK(guard.Apply(2147483647, false, true) == D::Stop);
            break;
        case 5:  // A stop received during startup wins before its queued apply.
            CHECK(guard.Apply(10, false, false) == D::Stop);
            CHECK(guard.Apply(11, true, false) == D::Start);
            CHECK(!VoiceLabRecordingGuard::StartSuperseded(11, 10));
            CHECK(!VoiceLabRecordingGuard::StartSuperseded(11, 11));
            CHECK(VoiceLabRecordingGuard::StartSuperseded(11, 12));
            CHECK(guard.Apply(12, false, false) == D::Stop);
            CHECK(guard.Apply(13, true, false) == D::Start);
            CHECK(!VoiceLabRecordingGuard::StartSuperseded(13, 12));
            CHECK(VoiceLabRecordingGuard::StartSuperseded(2147483646, 2147483647));
            break;
        default:
            return -1;
    }
    return 0;
}

#ifndef TEST_CASE
#define TEST_CASE 0
#endif
static_assert(run_recording_guard_test(TEST_CASE) == 0, "Recording guard scenario failed");
