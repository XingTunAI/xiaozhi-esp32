#include "voice_lab_media_policy.h"
using P = VoiceLabMediaPolicy;
static_assert(P::CanPump(true, false, true, false), "stopping capture retains media recovery");
static_assert(!P::CanPump(false, false, true, false), "background worker yields to stop owner");
static_assert(!P::CanPump(true, false, true, true), "abort must not resume media");
static_assert(!P::CanPump(true, false, false, false), "finished session must not reconnect");
static_assert(!P::TailConfirmed(320, true, true, 8000, 8000), "queued PCM must drain");
static_assert(!P::TailConfirmed(0, true, true, 7680, 8000), "socket success is not final ACK");
static_assert(!P::TailConfirmed(0, false, true, 8000, 8000), "end requires a live transport");
static_assert(!P::TailConfirmed(0, true, false, 8000, 8000), "reconnect must be accepted");
static_assert(P::TailConfirmed(0, true, true, 8000, 8000), "cutoff ACK permits end");
static_assert(P::BatchFrames(24, 0, false) == 0, "do not send per-frame packets");
static_assert(P::BatchFrames(25, 0, false) == 25, "500 ms batch");
static_assert(P::BatchFrames(600, 0, false) == 25, "bounded recovery packet");
static_assert(P::BatchFrames(7, 0, true) == 7, "flush short tail on stop");
static_assert(P::BatchFrames(0, 0, true) == 0, "no empty packet");
static_assert(P::BatchFrames(25, P::kWindowSamples, false) == 0, "full window waits");
static_assert(P::BatchFrames(25, P::kWindowSamples + 320, true) == 0, "no unsigned wrap");
static_assert(P::BatchFrames(25, P::kWindowSamples - 640, false) == 2, "never exceed window");
static_assert(P::BatchFrames(25, P::kWindowSamples - 1, true) == 0, "whole frames only");
constexpr bool AckReleasesWindow() {
    uint64_t sent = P::kWindowSamples;
    uint64_t ack = 0;
    if (P::BatchFrames(50, sent - ack, false) != 0)
        return false;
    ack = 8000;
    const auto next = P::BatchFrames(50, sent - ack, false);
    sent += next * P::kSamplesPerFrame;
    return next == 25 && sent - ack == P::kWindowSamples;
}
static_assert(AckReleasesWindow(), "ACK releases capacity without dropping queued capture");
