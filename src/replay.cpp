#include "ull/engine.hpp"
#include "ull/telemetry.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
void emit(const ull::Telemetry& event) {
    const auto& s = event.stats;
    std::cout << "{\"books\":" << s.books << ",\"committed_pairs\":" << s.committed_pairs
              << ",\"rejects\":" << s.rejects << ",\"position\":" << s.position
              << ",\"cash\":" << s.cash << ",\"pnl\":" << s.pnl
              << ",\"open_buy\":" << s.open_buy << ",\"open_sell\":" << s.open_sell
              << ",\"killed\":" << s.killed << ",\"feed_ready\":" << s.feed_ready
              << ",\"telemetry_dropped\":" << event.dropped
              << ",\"sample_ns\":" << event.sample_ns
              << ",\"cpu_latency_ns\":" << event.cpu_latency_ns << "}\n";
}
}

int main() {
    // A dead telemetry pipe must not deliver SIGPIPE to the trading thread.
    std::signal(SIGPIPE, SIG_IGN);
    ull::Engine engine;
    ull::SpscQueue<ull::Telemetry, 1024> queue;
    std::atomic<bool> done{false};
    std::thread monitor([&] {
        ull::Telemetry event;
        for (;;) {
            if (queue.pop(event)) emit(event);
            else if (done.load(std::memory_order_acquire)) {
                while (queue.pop(event)) emit(event);
                break;
            } else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::uint64_t dropped = 0;
    std::uint64_t now = 0;
    for (std::uint64_t i = 1; i <= 100'000; ++i) {
        now = i * 1'000;
        const auto bid = 10'000 + static_cast<std::int64_t>(i % 7);
        const ull::Book book{i, now, bid, bid + 3};
        const bool sampled = i % 256 == 0;
        const auto begin = sampled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (i == 1) engine.recover(book, now);
        else engine.on_book(book, now);
        std::array<ull::Order, 2> orders;
        const bool committed = engine.commit(engine.prepare(), now, orders);
        std::int64_t latency = -1;
        if (sampled) {
            asm volatile("" : : "g"(&engine), "g"(&orders) : "memory");
            latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count();
        }
        if (committed) {
            // Synthetic executions, NOT a fill model or profitability evidence.
            // Complete a round trip every third book; otherwise cancel both.
            if (i % 3 == 0) {
                for (const auto& order : orders)
                    engine.on_fill(order.id, order.quantity, order.price, 1);
            } else {
                for (const auto& order : orders) engine.on_terminal(order.id);
            }
        }
        if (sampled && !queue.push({engine.stats(), dropped, now, latency})) ++dropped;
    }
    done.store(true, std::memory_order_release);
    monitor.join(); // Shutdown may wait on stdout; processing above never does.
    emit({engine.stats(), dropped, now}); // Exact final totals, outside hot path.
    if (!std::cout) {
        std::cerr << "telemetry output failed (replay processing completed)\n";
        return 1;
    }
}
