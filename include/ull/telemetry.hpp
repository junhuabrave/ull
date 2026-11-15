#pragma once

#include "ull/engine.hpp"
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace ull {

// Bounded single-producer/single-consumer queue. No retry, allocation, syscall,
// clock read or exporter callback on push. The producer owns its drop counter.
template <typename T, std::size_t N>
class SpscQueue {
    static_assert(N >= 2 && (N & (N - 1)) == 0);
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
    alignas(64) std::array<T, N> data_{};
public:
    bool push(const T& value) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto next = (write + 1) & (N - 1);
        if (next == read_.load(std::memory_order_acquire)) return false;
        data_[write] = value;
        write_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& value) noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        value = data_[read];
        read_.store((read + 1) & (N - 1), std::memory_order_release);
        return true;
    }
};

struct Telemetry {
    Stats stats{};
    std::uint64_t dropped = 0;
    std::uint64_t sample_ns = 0;
    std::int64_t cpu_latency_ns = -1; // -1 means accounting-only snapshot.
};
} // namespace ull
