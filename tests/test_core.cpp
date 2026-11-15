#include "ull/engine.hpp"
#include "ull/telemetry.hpp"
#include <cstdlib>
#include <iostream>
#include <thread>

#define CHECK(expr) do { if (!(expr)) { std::cerr << __LINE__ << ": " #expr "\n"; std::abort(); } } while (false)

using Pair = std::array<ull::Order, 2>;

void freshness_and_speculation() {
    ull::Engine e({.max_book_age_ns = 10});
    Pair orders{};
    CHECK(!e.commit(e.prepare(), 100, orders));
    CHECK(e.recover({10, 100, 101, 104}, 100));
    auto candidate = e.prepare();
    CHECK(!e.commit(candidate, 99, orders));
    CHECK(!e.commit(candidate, 111, orders));
    CHECK(e.commit(candidate, 110, orders)); // Inclusive age boundary.
    CHECK(orders[0].price == 101 && orders[1].price == 104);
    CHECK(orders[0].quantity == 5 && orders[1].quantity == 5);
    CHECK(!e.commit(candidate, 110, orders)); // Cannot commit twice.
    candidate = e.prepare();
    CHECK(e.on_book({11, 110, 103, 108}, 110));
    CHECK(!e.commit(candidate, 110, orders));
    CHECK(!e.on_book({13, 111, 103, 108}, 111)); // Gap latches feed off.
    CHECK(!e.on_book({12, 112, 103, 108}, 112));
    CHECK(!e.commit(e.prepare(), 112, orders));
    CHECK(!e.recover({20, 113, 103, 103}, 113));
    CHECK(!e.recover({20, 114, 103, 108}, 113)); // Future timestamp.
    CHECK(e.recover({20, 113, 103, 108}, 113));
    ull::Engine other;
    CHECK(other.recover({20, 113, 103, 108}, 113));
    CHECK(!other.commit(e.prepare(), 113, orders));
    candidate = e.prepare();
    e.kill();
    CHECK(!e.commit(candidate, 113, orders));
    CHECK(e.recover({21, 114, 103, 108}, 114));
    CHECK(!e.commit(e.prepare(), 114, orders)); // Recovery cannot clear kill.
}

void reservations_and_pnl() {
    ull::Engine e({.max_position = 10, .order_quantity = 5});
    CHECK(e.recover({1, 1, 100, 105}, 1)); // floor midpoint is 102.
    Pair a{}, b{}, scratch{};
    CHECK(e.commit(e.prepare(), 1, a));
    auto before_fill = e.prepare();
    CHECK(e.on_fill(a[0].id, 3, 99, 2)); // Buy with price improvement.
    CHECK(e.stats().position == 3 && e.stats().cash == -299 && e.stats().pnl == 7);
    CHECK(e.stats().open_buy == 2 && e.stats().open_sell == 5);
    CHECK(!e.on_fill(a[0].id, 3, 99, 2)); // Duplicate fee not charged.
    CHECK(!e.on_fill(a[0].id, 2, 99, 2));
    CHECK(e.stats().cash == -299);
    CHECK(!e.commit(before_fill, 1, scratch));
    CHECK(e.commit(e.prepare(), 1, b)); // 3 + 2 + 5 = 10 exactly.
    CHECK(!e.commit(e.prepare(), 1, scratch)); // Both resting sides count independently.
    CHECK(e.on_fill(a[1].id, 5, 106, 1));
    CHECK(e.stats().position == -2 && e.stats().cash == 230 && e.stats().pnl == 26);
    CHECK(e.on_terminal(a[0].id));
    CHECK(!e.on_terminal(a[0].id));
    CHECK(e.on_terminal(b[0].id) && e.on_terminal(b[1].id));
    CHECK(e.stats().open_buy == 0 && e.stats().open_sell == 0);
    CHECK(e.on_book({2, 2, 110, 115}, 2));
    CHECK(e.stats().pnl == 6); // Short inventory is marked, not just cash.

    ull::Engine short_limit({.max_position = 5, .order_quantity = 5});
    CHECK(short_limit.recover({1, 1, 100, 105}, 1));
    CHECK(short_limit.commit(short_limit.prepare(), 1, a));
    CHECK(short_limit.on_fill(a[1].id, 5, 105));
    CHECK(!short_limit.commit(short_limit.prepare(), 1, b));
}

void risk_limits() {
    Pair orders{};
    ull::Engine loss({.max_loss = 8});
    CHECK(loss.recover({1, 1, 100, 104}, 1));
    CHECK(loss.commit(loss.prepare(), 1, orders));
    CHECK(loss.on_fill(orders[0].id, 5, 100, 3));
    CHECK(loss.on_book({2, 2, 97, 101}, 2)); // -503 + 5*99 = -8.
    CHECK(loss.stats().pnl == -8 && loss.stats().killed);
    CHECK(loss.stats().open_sell == 5); // Kill never releases resting risk.
    CHECK(!loss.commit(loss.prepare(), 2, orders));

    ull::Engine notional({.max_order_notional = 520});
    CHECK(notional.recover({1, 1, 100, 104}, 1));
    CHECK(notional.commit(notional.prepare(), 1, orders));
    CHECK(notional.on_book({2, 2, 100, 105}, 2));
    CHECK(!notional.commit(notional.prepare(), 2, orders));
    CHECK(!notional.on_fill(orders[0].id, 6, 100));
    CHECK(notional.stats().killed && notional.stats().position == 0);

    ull::Engine full({.max_position = 1000});
    CHECK(full.recover({1, 1, 100, 104}, 1));
    Pair first{};
    for (int i = 0; i < 32; ++i) {
        CHECK(full.commit(full.prepare(), 1, orders));
        if (i == 0) first = orders;
    }
    CHECK(!full.commit(full.prepare(), 1, orders));
    CHECK(full.on_terminal(first[0].id) && full.on_terminal(first[1].id));
    CHECK(full.commit(full.prepare(), 1, orders));
    CHECK(orders[0].id != first[0].id);
    CHECK(!full.on_fill(first[0].id, 5, 100)); // Old ID cannot hit reused slot.
    CHECK(full.stats().position == 0 && full.stats().open_buy == 160);

    bool threw = false;
    try { ull::Engine invalid({.max_position = 0}); }
    catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

void queue_contract() {
    ull::SpscQueue<std::uint64_t, 4> small;
    std::uint64_t value = 0;
    CHECK(!small.pop(value));
    CHECK(small.push(7) && small.push(9) && small.push(13));
    CHECK(!small.push(17)); // Full drops, never overwrites unread data.
    CHECK(small.pop(value) && value == 7);
    CHECK(small.push(19));
    CHECK(small.pop(value) && value == 9);
    CHECK(small.pop(value) && value == 13);
    CHECK(small.pop(value) && value == 19);
    CHECK(!small.pop(value));

    ull::SpscQueue<std::uint64_t, 256> concurrent;
    constexpr std::uint64_t count = 1'000'000;
    std::thread consumer([&] {
        for (std::uint64_t expected = 1; expected <= count; ++expected) {
            std::uint64_t received = 0;
            while (!concurrent.pop(received)) std::this_thread::yield();
            CHECK(received == expected);
        }
    });
    for (std::uint64_t i = 1; i <= count; ++i)
        while (!concurrent.push(i)) std::this_thread::yield();
    consumer.join();
}

int main() {
    freshness_and_speculation();
    reservations_and_pnl();
    risk_limits();
    queue_contract();
    std::cout << "PASS: freshness/speculation, reservations/P&L, risk limits, SPSC ordering/overflow\n";
}
