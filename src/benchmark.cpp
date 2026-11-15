#include "ull/engine.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

using Clock = std::chrono::steady_clock;

// Compiler barrier: preserve the complete engine result inside the timed region.
inline void observe(const ull::Engine& engine, const std::array<ull::Order, 2>& orders) {
    asm volatile("" : : "g"(&engine), "g"(&orders) : "memory");
}

int main() {
    constexpr std::size_t count = 200'000;
    constexpr std::size_t warmup = 10'000;
    std::vector<std::int64_t> samples(count), clock_cost(count);
    ull::Engine engine;
    engine.recover({1, 1, 10'000, 10'003}, 1);
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < count + warmup; ++i) {
        const auto seq = static_cast<std::uint64_t>(i + 2);
        const auto bid = 10'000 + static_cast<std::int64_t>(i % 19);
        const ull::Book book{seq, seq, bid, bid + 3};
        std::array<ull::Order, 2> orders;
        const auto begin = Clock::now();
        const bool updated = engine.on_book(book, seq);
        const bool sent = engine.commit(engine.prepare(), seq, orders);
        observe(engine, orders);
        const auto end = Clock::now();
        if (!updated || !sent) return 1;
        if (i >= warmup)
            samples[i - warmup] = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
        checksum += orders[0].id + static_cast<std::uint64_t>(orders[1].price);
        for (const auto& order : orders) engine.on_terminal(order.id);
    }
    for (auto& sample : clock_cost) {
        const auto begin = Clock::now();
        asm volatile("" : : : "memory");
        const auto end = Clock::now();
        sample = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    }
    std::sort(samples.begin(), samples.end());
    std::sort(clock_cost.begin(), clock_cost.end());
    std::cout << "CPU ONLY: normalized book -> quote -> risk -> reserved order pair; no network, encoding or telemetry\n"
              << "samples=" << count << " warmup=" << warmup
              << " p50_ns=" << samples[count / 2] << " p99_ns=" << samples[count * 99 / 100]
              << " p999_ns=" << samples[count * 999 / 1000] << " max_ns=" << samples.back()
              << " clock_pair_p50_ns=" << clock_cost[count / 2] << " checksum=" << checksum << '\n'
              << "Uncorrected steady_clock intervals on this host; not wire-to-wire latency.\n";
}
