#ifndef VOICE_LAB_RECORDING_LEASE_H
#define VOICE_LAB_RECORDING_LEASE_H

#include <cstdint>

// Monotonic-time rules shared by firmware and host tests. The caller serializes
// access and independently closes capture at Deadline(), even if I/O blocks.
class VoiceLabRecordingLease {
public:
    static constexpr bool MatchesRequest(bool same_id, int64_t deadline, int64_t now) {
        return same_id && deadline > 0 && now < deadline;
    }

    static constexpr bool ValidDuration(int64_t lease_ms, int64_t maximum_ms) {
        return lease_ms >= 1000 && lease_ms <= 30000 && maximum_ms >= 60000 &&
               maximum_ms <= 3600000;
    }

    constexpr bool Begin(int64_t received_at, int64_t now, int64_t lease_ms, int64_t maximum_ms) {
        if (!ValidDuration(lease_ms, maximum_ms) || received_at < 0 || now < received_at ||
            now >= received_at + lease_ms * 1000)
            return false;
        maximum_deadline_ = received_at + maximum_ms * 1000;
        deadline_ = received_at + lease_ms * 1000;
        next_renewal_at_ = received_at + RenewalInterval(lease_ms);
        pending_id_ = 0;
        return true;
    }

    constexpr void Clear() {
        deadline_ = 0;
        maximum_deadline_ = 0;
        pending_id_ = 0;
    }

    constexpr bool Active() const { return deadline_ > 0; }
    constexpr bool Expired(int64_t now) const { return Active() && now >= deadline_; }
    constexpr int64_t Deadline() const { return deadline_; }
    constexpr int64_t MaximumDeadline() const { return maximum_deadline_; }

    constexpr uint64_t RequestRenewal(int64_t now) {
        if (!Active() || Expired(now) || now < next_renewal_at_)
            return 0;
        pending_id_ = ++sequence_;
        pending_sent_at_ = now;
        next_renewal_at_ = now + 10000000;
        return pending_id_;
    }

    constexpr bool Renew(int64_t now, uint64_t id, int64_t lease_ms) {
        if (!Active() || Expired(now) || !id || id != pending_id_ || now < pending_sent_at_ ||
            now >= pending_sent_at_ + 10000000 || lease_ms < 1000 || lease_ms > 30000)
            return false;
        const auto proposed = pending_sent_at_ + lease_ms * 1000;
        if (now >= proposed)
            return false;
        // Delay on the wire never grants extra capture time. A response can
        // shorten a lease, but a duplicate cannot extend it again.
        deadline_ = proposed < maximum_deadline_ ? proposed : maximum_deadline_;
        pending_id_ = 0;
        next_renewal_at_ = pending_sent_at_ + RenewalInterval(lease_ms);
        return true;
    }

private:
    static constexpr int64_t RenewalInterval(int64_t lease_ms) {
        return lease_ms * 1000 / 3 < 10000000 ? lease_ms * 1000 / 3 : 10000000;
    }
    int64_t deadline_ = 0;
    int64_t maximum_deadline_ = 0;
    int64_t next_renewal_at_ = 0;
    int64_t pending_sent_at_ = 0;
    uint64_t sequence_ = 0;
    uint64_t pending_id_ = 0;
};

#endif
