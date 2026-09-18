#include "tls_write_policy.h"

using P = TlsWritePolicy;
constexpr bool Retryable(int result) { return result == -2 || result == -3; }

constexpr bool RetryKeepsArguments() {
    int call = 0;
    bool valid = true;
    auto result = P::Send(
        P::kChunkBytes + 512, [] { return int64_t{0}; },
        [&](size_t offset, size_t count) {
            valid = valid && offset == (call < 2 ? 0u : 512u) && count == P::kChunkBytes;
            ++call;
            return call == 1   ? -2
                   : call == 2 ? 512
                   : call == 3 ? -3
                               : static_cast<int>(P::kChunkBytes);
        },
        Retryable, [] {});
    return valid && call == 4 && result.written == P::kChunkBytes + 512 && !result.error &&
           !result.timed_out;
}
static_assert(RetryKeepsArguments(),
              "WANT retries retain pointer/length; partial writes advance once");

constexpr bool TimeoutWithoutProgress() {
    int64_t clock = 0;
    int calls = 0;
    auto result = P::Send(
        16048, [&] { return clock; },
        [&](size_t, size_t) {
            ++calls;
            clock += 250000;
            return -2;
        },
        Retryable, [&] { clock += 10000; });
    return result.timed_out && !result.error && result.written == 0 && calls == 12;
}
static_assert(TimeoutWithoutProgress(), "repeated WANT must reach the original deadline");

constexpr bool NonblockingBackpressureDeadline() {
    int64_t clock = 0;
    int calls = 0, yields = 0;
    bool arguments_unchanged = true;
    const auto result = P::Send(
        16048, [&] { return clock; },
        [&](size_t offset, size_t count) {
            arguments_unchanged = arguments_unchanged && offset == 0 && count == P::kChunkBytes;
            return ++calls % 2 ? -2 : -3;
        },
        Retryable,
        [&] {
            ++yields;
            clock += 10000;
        });
    return arguments_unchanged && result.timed_out && !result.error && result.written == 0 &&
           clock == P::kBudgetUs && calls == 300 && yields == calls;
}
static_assert(NonblockingBackpressureDeadline(),
              "immediate WANT_READ/WRITE yields without renewing the deadline or advancing data");

constexpr bool ProgressDoesNotRenewBudget() {
    int64_t clock = 0;
    auto result = P::Send(
        P::kChunkBytes * 4, [&] { return clock; },
        [&](size_t, size_t count) {
            clock += 1100000;
            return static_cast<int>(count);
        },
        Retryable, [] {});
    return result.timed_out && result.written == P::kChunkBytes * 3;
}
static_assert(ProgressDoesNotRenewBudget(), "successful chunks must not renew the send budget");

constexpr bool LateFinalWriteFails() {
    int64_t clock = 0;
    auto result = P::Send(
        512, [&] { return clock; },
        [&](size_t, size_t count) {
            clock += P::kBudgetUs;
            return static_cast<int>(count);
        },
        Retryable, [] {});
    return result.timed_out && result.written == 512;
}
static_assert(LateFinalWriteFails(), "deadline also applies to the final TLS call");

constexpr bool FatalAndZeroStop() {
    int calls = 0;
    auto fatal = P::Send(
        16048, [] { return int64_t{0}; }, [&](size_t, size_t) { return ++calls == 1 ? 100 : -7; },
        Retryable, [] {});
    auto zero = P::Send(
        1024, [] { return int64_t{0}; }, [](size_t, size_t) { return 0; }, Retryable, [] {});
    return calls == 2 && fatal.written == 100 && fatal.error == -7 && !fatal.timed_out &&
           zero.error && !zero.timed_out && zero.written == 0;
}
static_assert(FatalAndZeroStop(), "failed writes cannot spin or commit unsent bytes");

constexpr bool ChunksAndTail() {
    size_t maximum = 0, last = 0;
    auto result = P::Send(
        16048, [] { return int64_t{0}; },
        [&](size_t, size_t count) {
            maximum = count > maximum ? count : maximum;
            last = count;
            return static_cast<int>(count);
        },
        Retryable, [] {});
    return maximum == P::kChunkBytes && last == 16048 % P::kChunkBytes && result.written == 16048 &&
           !result.error && !result.timed_out;
}
static_assert(ChunksAndTail(),
              "full websocket message is covered by bounded chunks and exact tail");

constexpr bool EmptyDoesNotWrite() {
    auto result =
        P::Send(0, [] { return int64_t{0}; }, [](size_t, size_t) { return -7; }, Retryable, [] {});
    return !result.error && !result.timed_out && result.written == 0;
}
static_assert(EmptyDoesNotWrite(), "empty message does not call TLS");
