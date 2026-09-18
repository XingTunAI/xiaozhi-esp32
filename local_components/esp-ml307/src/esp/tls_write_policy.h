#pragma once

#include <stddef.h>
#include <stdint.h>

// Keep each ESP-TLS call within one record: its internal fragmentation loop
// otherwise hides repeated socket waits from the caller's deadline checks.
struct TlsWritePolicy {
    static constexpr size_t kChunkBytes = 1024;
    static constexpr int kSocketWaitMs = 250;
    static constexpr int64_t kBudgetUs = 3000000;

    struct Result {
        size_t written;
        int error;
        bool timed_out;
    };

    template <typename Clock, typename Write, typename Retryable, typename Yield>
    static constexpr Result Send(size_t size, Clock now, Write write, Retryable retryable,
                                 Yield yield) {
        const auto deadline = now() + kBudgetUs;
        size_t written = 0;
        while (written < size) {
            if (now() >= deadline)
                return {written, 0, true};
            const size_t remaining = size - written;
            const size_t count = remaining < kChunkBytes ? remaining : kChunkBytes;
            const int result = write(written, count);
            if (retryable(result)) {
                // A TLS WANT retry must retain the exact data pointer and size.
                yield();
                continue;
            }
            if (result <= 0 || static_cast<size_t>(result) > count)
                return {written, result < 0 ? result : -1, false};
            written += static_cast<size_t>(result);
            // Also detect a final write that returned after the budget expired.
            if (now() >= deadline)
                return {written, 0, true};
        }
        return {written, 0, false};
    }
};
