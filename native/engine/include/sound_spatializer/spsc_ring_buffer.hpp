#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace sound_spatializer {

template <typename T>
class SpscRingBuffer {
    static_assert(std::is_trivially_copyable_v<T>, "The real-time FIFO stores trivially copyable values");
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "The x64 real-time FIFO requires lock-free size_t atomics");

public:
    explicit SpscRingBuffer(std::size_t requested_capacity)
        : capacity_(std::bit_ceil(requested_capacity < 2 ? std::size_t{2} : requested_capacity)),
          mask_(capacity_ - 1),
          data_(std::make_unique<T[]>(capacity_)) {}

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    [[nodiscard]] bool try_push(const T& value) noexcept {
        const std::size_t write = write_index_.load(std::memory_order_relaxed);
        const std::size_t next = write + 1;
        if (next - read_index_.load(std::memory_order_acquire) > capacity_) {
            return false;
        }
        data_[write & mask_] = value;
        write_index_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t push(const T* values, std::size_t count) noexcept {
        std::size_t pushed = 0;
        while (pushed < count && try_push(values[pushed])) {
            ++pushed;
        }
        return pushed;
    }

    [[nodiscard]] bool try_pop(T& value) noexcept {
        const std::size_t read = read_index_.load(std::memory_order_relaxed);
        if (read == write_index_.load(std::memory_order_acquire)) {
            return false;
        }
        value = data_[read & mask_];
        read_index_.store(read + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::size_t pop(T* values, std::size_t count) noexcept {
        std::size_t popped = 0;
        while (popped < count && try_pop(values[popped])) {
            ++popped;
        }
        return popped;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t write = write_index_.load(std::memory_order_acquire);
        const std::size_t read = read_index_.load(std::memory_order_acquire);
        return write - read;
    }

    // Consumer-side bounded discard. Moving only the read cursor preserves the
    // SPSC ownership model and lets a real-time consumer shed stale backlog in
    // O(1), without touching or copying the stored frames.
    [[nodiscard]] std::size_t discard_oldest(std::size_t count) noexcept {
        const std::size_t read = read_index_.load(std::memory_order_relaxed);
        const std::size_t write = write_index_.load(std::memory_order_acquire);
        const std::size_t discarded = std::min(count, write - read);
        read_index_.store(read + discarded, std::memory_order_release);
        return discarded;
    }

    // A producer may snapshot this monotonic cursor before publishing a
    // discontinuous packet. The consumer can subsequently discard exactly the
    // older portion while retaining frames written after the boundary.
    [[nodiscard]] std::size_t producer_sequence() const noexcept {
        return write_index_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t discard_before(std::size_t producer_sequence) noexcept {
        const std::size_t read = read_index_.load(std::memory_order_relaxed);
        const std::size_t write = write_index_.load(std::memory_order_acquire);
        const std::size_t target =
            producer_sequence < read ? read : std::min(producer_sequence, write);
        read_index_.store(target, std::memory_order_release);
        return target - read;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    void reset() noexcept {
        const std::size_t write = write_index_.load(std::memory_order_acquire);
        read_index_.store(write, std::memory_order_release);
    }

private:
    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<T[]> data_;
    alignas(64) std::atomic<std::size_t> read_index_{};
    alignas(64) std::atomic<std::size_t> write_index_{};
};

} // namespace sound_spatializer
