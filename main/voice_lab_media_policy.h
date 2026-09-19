#pragma once
#include <stddef.h>
#include <stdint.h>

// VLA2 uses 20 ms logical frames. Match the existing 500 ms device baseline;
// a full ACK window applies backpressure to the media worker, never to capture.
struct VoiceLabMediaPolicy {
    static constexpr size_t kSamplesPerFrame = 320;
    static constexpr size_t kBatchFrames = 25;
    static constexpr uint64_t kWindowSamples = 16000 * 4;

    static constexpr bool CanPump(bool draining, bool recording, bool tail_pending,
                                  bool interrupted) {
        return draining ? tail_pending && !interrupted : recording;
    }

    static constexpr bool TailConfirmed(size_t pending_samples, bool connected, bool accepted,
                                        uint64_t acknowledged, uint64_t cutoff) {
        return pending_samples == 0 && connected && accepted && acknowledged >= cutoff;
    }

    static constexpr size_t BatchFrames(size_t pending_frames, uint64_t outstanding,
                                        bool draining) {
        if (outstanding >= kWindowSamples || (!draining && pending_frames < kBatchFrames))
            return 0;
        const auto room = static_cast<size_t>((kWindowSamples - outstanding) / kSamplesPerFrame);
        const auto batch = pending_frames < kBatchFrames ? pending_frames : kBatchFrames;
        return batch < room ? batch : room;
    }

    // A TF-backed discontinuity starts a new live window without pretending the
    // missing interval was acknowledged. The original sample/sequence gap remains.
    static constexpr uint64_t OutstandingSamples(uint64_t sent, uint64_t acknowledged,
                                                 uint64_t live_start) {
        const auto committed = acknowledged > live_start ? acknowledged : live_start;
        return sent > committed ? sent - committed : 0;
    }
};
