#include "voice_lab_recording_lease.h"

constexpr int run_recording_lease_test(int scenario) {
    VoiceLabRecordingLease lease;
#define CHECK(expression) \
    if (!(expression))    \
    return __LINE__
    switch (scenario) {
        case 0:  // A request must match and still be pending; expiry is inclusive.
            CHECK(VoiceLabRecordingLease::MatchesRequest(true, 10000000, 9999999));
            CHECK(!VoiceLabRecordingLease::MatchesRequest(true, 10000000, 10000000));
            CHECK(!VoiceLabRecordingLease::MatchesRequest(false, 10000000, 1));
            CHECK(!VoiceLabRecordingLease::MatchesRequest(true, 0, 1));
            break;
        case 1:  // Worker/announcement delays consume the initial lease.
            CHECK(lease.Begin(1000000, 6000000, 30000, 3600000));
            CHECK(lease.Deadline() == 31000000);
            CHECK(lease.MaximumDeadline() == 3601000000LL);
            CHECK(!lease.Expired(30999999));
            CHECK(lease.Expired(31000000));
            CHECK(!lease.Begin(1000000, 31000000, 30000, 60000));
            CHECK(!lease.Begin(1000000, 0, 30000, 60000));
            break;
        case 2: {  // A timely matching reply extends from request time, not arrival.
            CHECK(lease.Begin(0, 0, 30000, 3600000));
            CHECK(lease.RequestRenewal(9999999) == 0);
            const auto first = lease.RequestRenewal(10000000);
            CHECK(first == 1);
            CHECK(lease.Renew(19000000, first, 30000));
            CHECK(lease.Deadline() == 40000000);
            CHECK(!lease.Renew(19000001, first, 30000));
            CHECK(lease.Deadline() == 40000000);
            break;
        }
        case 3: {  // Expired/unsolicited responses never restore authorization.
            CHECK(lease.Begin(0, 0, 30000, 60000));
            CHECK(!lease.Renew(1, 1, 30000));
            const auto first = lease.RequestRenewal(10000000);
            CHECK(!lease.Renew(20000000, first, 30000));
            CHECK(lease.Deadline() == 30000000);
            const auto second = lease.RequestRenewal(21000000);
            CHECK(second == 2);
            CHECK(!lease.Renew(22000000, first, 30000));
            CHECK(!lease.Renew(30000000, second, 30000));
            CHECK(lease.RequestRenewal(30000000) == 0);
            break;
        }
        case 4:  // Repeated renewals cannot move the absolute session maximum.
            CHECK(lease.Begin(0, 0, 30000, 60000));
            for (int64_t time = 10000000; time <= 50000000; time += 10000000) {
                const auto id = lease.RequestRenewal(time);
                CHECK(id > 0);
                CHECK(lease.Renew(time, id, 30000));
            }
            CHECK(lease.Deadline() == 60000000);
            CHECK(lease.Expired(60000000));
            break;
        case 5: {  // Closing/resetting a session invalidates its old renewal id.
            CHECK(lease.Begin(0, 0, 30000, 60000));
            const auto first = lease.RequestRenewal(10000000);
            lease.Clear();
            CHECK(!lease.Active());
            CHECK(!lease.Renew(11000000, first, 30000));
            CHECK(lease.Begin(12000000, 12000000, 30000, 60000));
            const auto second = lease.RequestRenewal(22000000);
            CHECK(second > first);
            CHECK(!lease.Renew(23000000, first, 30000));
            CHECK(lease.Renew(23000000, second, 30000));
            break;
        }
        case 6: {  // Bounds reject invalid network values, shorter grants take effect.
            CHECK(!lease.Begin(0, 0, 999, 60000));
            CHECK(!lease.Begin(0, 0, 30001, 60000));
            CHECK(!lease.Begin(0, 0, 30000, 59999));
            CHECK(!lease.Begin(0, 0, 30000, 3600001));
            CHECK(lease.Begin(0, 0, 30000, 60000));
            const auto id = lease.RequestRenewal(10000000);
            CHECK(!lease.Renew(11000000, id, 1000));
            CHECK(lease.Renew(11000000, id, 5000));
            CHECK(lease.Deadline() == 15000000);
            CHECK(lease.RequestRenewal(11666666) > 0);
            break;
        }
        default:
            return -1;
    }
    return 0;
}

#ifndef TEST_CASE
#define TEST_CASE 0
#endif
static_assert(run_recording_lease_test(TEST_CASE) == 0, "Recording lease scenario failed");
