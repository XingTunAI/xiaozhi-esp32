#ifndef VOICE_LAB_PCM_BUFFER_H
#define VOICE_LAB_PCM_BUFFER_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>

#include <esp_heap_caps.h>

// The caller serializes producer/consumer access. Pop only copies the requested
// frame, so recovering a backlog never moves all the remaining audio samples.
class VoiceLabPcmBuffer {
public:
    bool Allocate(size_t capacity, uint32_t caps) {
        if (capacity == 0)
            return false;
        if (!storage_ || capacity_ != capacity) {
            auto* data = static_cast<int16_t*>(heap_caps_malloc(capacity * sizeof(int16_t), caps));
            if (!data)
                return false;
            storage_.reset(data);
            capacity_ = capacity;
        }
        clear();
        return true;
    }

    void clear() { head_ = size_ = 0; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    bool Push(const int16_t* source, size_t count) {
        if (!storage_ || count > capacity_ - size_)
            return false;
        const auto tail = (head_ + size_) % capacity_;
        const auto first = std::min(count, capacity_ - tail);
        std::memcpy(storage_.get() + tail, source, first * sizeof(int16_t));
        std::memcpy(storage_.get(), source + first, (count - first) * sizeof(int16_t));
        size_ += count;
        return true;
    }

    bool Pop(int16_t* destination, size_t count) {
        if (!storage_ || count > size_)
            return false;
        const auto first = std::min(count, capacity_ - head_);
        std::memcpy(destination, storage_.get() + head_, first * sizeof(int16_t));
        std::memcpy(destination + first, storage_.get(), (count - first) * sizeof(int16_t));
        head_ = (head_ + count) % capacity_;
        size_ -= count;
        return true;
    }

private:
    struct Deleter {
        void operator()(int16_t* data) const { heap_caps_free(data); }
    };
    std::unique_ptr<int16_t[], Deleter> storage_;
    size_t capacity_ = 0;
    size_t head_ = 0;
    size_t size_ = 0;
};

#endif
