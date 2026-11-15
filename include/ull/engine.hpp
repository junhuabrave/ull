#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace ull {

// Single instrument, single owner thread. Prices are positive integer ticks;
// quantities are integer lots; cash, fees and P&L are tick-lots, not currency.
struct Limits {
    std::int64_t max_position = 100;
    std::int64_t order_quantity = 5;
    std::int64_t max_order_notional = 10'000'000;
    std::int64_t max_loss = 10'000;
    std::uint64_t max_book_age_ns = 1'000'000;
};

struct Book {
    std::uint64_t sequence = 0;
    std::uint64_t received_ns = 0;
    std::int64_t bid = 0;
    std::int64_t ask = 0;
};

enum class Side { buy, sell };

struct Order {
    std::uint64_t id = 0;
    Side side = Side::buy;
    std::int64_t price = 0;
    std::int64_t quantity = 0;
    std::int64_t filled = 0;
};

// A prepared candidate is deliberately opaque: only this engine can construct
// or validate it. Preparation has no reservation or transmission side effects.
class Prepared {
    friend class Engine;
    const void* owner_ = nullptr;
    std::uint64_t generation_ = 0;
    std::uint64_t risk_epoch_ = 0;
    std::int64_t bid_ = 0;
    std::int64_t ask_ = 0;
};

struct Stats {
    std::uint64_t books = 0;
    std::uint64_t committed_pairs = 0;
    std::uint64_t rejects = 0;
    std::int64_t position = 0;
    std::int64_t cash = 0;
    std::int64_t pnl = 0;
    std::int64_t open_buy = 0;
    std::int64_t open_sell = 0;
    bool killed = false;
    bool feed_ready = false;
};

class Engine {
public:
    explicit Engine(Limits limits = {}) : limits_(limits) {
        // These bounds also bound every price/quantity/position product.
        if (limits.max_position <= 0 || limits.max_position > 1'000'000 ||
            limits.order_quantity <= 0 || limits.order_quantity > limits.max_position ||
            limits.max_order_notional <= 0 || limits.max_loss <= 0 ||
            limits.max_book_age_ns == 0)
            throw std::invalid_argument("invalid risk limits");
    }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Only a validated recovery snapshot may restore a broken feed. The caller
    // must reconcile snapshot/incremental sequencing before calling this.
    bool recover(Book book, std::uint64_t now_ns) noexcept {
        ++generation_;
        stats_.feed_ready = valid_book(book, now_ns);
        if (stats_.feed_ready) {
            book_ = book;
            ++stats_.books;
            mark();
        }
        return stats_.feed_ready;
    }

    bool on_book(Book book, std::uint64_t now_ns) noexcept {
        ++generation_;
        if (!stats_.feed_ready || book_.sequence == UINT64_MAX ||
            book.sequence != book_.sequence + 1 || !valid_book(book, now_ns)) {
            stats_.feed_ready = false;
            return false;
        }
        book_ = book;
        ++stats_.books;
        mark();
        return true;
    }

    [[nodiscard]] Prepared prepare() const noexcept {
        Prepared result;
        result.owner_ = this;
        result.generation_ = generation_;
        result.risk_epoch_ = risk_epoch_;
        result.bid_ = book_.bid;
        result.ask_ = book_.ask;
        return result;
    }

    // This is a reservation + simulated handoff, NOT a NIC send. An actual
    // transport must keep reservations until definitive reject/cancel/fill.
    bool commit(const Prepared& candidate, std::uint64_t now_ns,
                std::array<Order, 2>& output) noexcept {
        const auto qty = limits_.order_quantity;
        const auto id = next_id_;
        if (candidate.owner_ != this || candidate.generation_ != generation_ ||
            candidate.risk_epoch_ != risk_epoch_ || stats_.killed ||
            !stats_.feed_ready || !valid_book(book_, now_ns) ||
            stats_.position + stats_.open_buy + qty > limits_.max_position ||
            stats_.position - stats_.open_sell - qty < -limits_.max_position ||
            candidate.bid_ * qty > limits_.max_order_notional ||
            candidate.ask_ * qty > limits_.max_order_notional ||
            id > UINT64_MAX - 2 || orders_[id % capacity].id != 0 ||
            orders_[(id + 1) % capacity].id != 0) {
            ++stats_.rejects;
            return false;
        }
        output = {Order{id, Side::buy, candidate.bid_, qty, 0},
                  Order{id + 1, Side::sell, candidate.ask_, qty, 0}};
        orders_[id % capacity] = output[0];
        orders_[(id + 1) % capacity] = output[1];
        stats_.open_buy += qty;
        stats_.open_sell += qty;
        next_id_ += 2;
        ++risk_epoch_; // No second commit from the same speculation.
        ++stats_.committed_pairs;
        return true;
    }

    // Ordered, cumulative execution reports make retransmission idempotent.
    // price and fee apply only to the newly reported quantity. A real adapter
    // must detect missing/out-of-order executions before calling this method.
    bool on_fill(std::uint64_t id, std::int64_t cumulative,
                 std::int64_t price, std::int64_t fee = 0) noexcept {
        auto& order = orders_[id % capacity];
        if (id == 0 || order.id != id) return false;
        if (cumulative <= order.filled) return false;
        if (cumulative > order.quantity || price <= 0 || price > max_price ||
            fee < 0 || fee > max_cash_delta ||
            (order.side == Side::buy && price > order.price) ||
            (order.side == Side::sell && price < order.price)) {
            kill(); // Adapter/account state is suspect; require reconciliation.
            return false;
        }
        const auto delta = cumulative - order.filled;
        const auto signed_qty = order.side == Side::buy ? delta : -delta;
        const auto cash_delta = -signed_qty * price - fee;
        if ((cash_delta > 0 && stats_.cash > INT64_MAX - cash_delta) ||
            (cash_delta < 0 && stats_.cash < INT64_MIN - cash_delta)) {
            kill();
            return false;
        }
        stats_.cash += cash_delta;
        stats_.position += signed_qty;
        (order.side == Side::buy ? stats_.open_buy : stats_.open_sell) -= delta;
        order.filled = cumulative;
        if (cumulative == order.quantity) order.id = 0;
        ++risk_epoch_;
        mark();
        return true;
    }

    // Call ONLY for a terminal exchange acknowledgement after processing its
    // cumulative fills, never merely on a cancel request or send timeout.
    bool on_terminal(std::uint64_t id) noexcept {
        auto& order = orders_[id % capacity];
        if (id == 0 || order.id != id) return false;
        (order.side == Side::buy ? stats_.open_buy : stats_.open_sell) -=
            order.quantity - order.filled;
        order.id = 0;
        ++risk_epoch_;
        return true;
    }

    // Latching kill prevents NEW orders. It does not pretend resting orders
    // disappeared. Cancellation, disconnect handling and recovery are adapter work.
    void kill() noexcept { stats_.killed = true; ++risk_epoch_; }
    [[nodiscard]] Stats stats() const noexcept { return stats_; }

private:
    static constexpr std::size_t capacity = 64;
    static constexpr std::int64_t max_price = 1'000'000'000;
    static constexpr std::int64_t max_cash_delta = 1'000'000'000'000'000;
    Limits limits_;
    Book book_{};
    Stats stats_{};
    std::array<Order, capacity> orders_{};
    std::uint64_t generation_ = 0;
    std::uint64_t risk_epoch_ = 0;
    std::uint64_t next_id_ = 1;

    bool valid_book(const Book& book, std::uint64_t now) const noexcept {
        return book.sequence != 0 && book.bid > 0 && book.ask > book.bid &&
               book.ask <= max_price && book.received_ns <= now &&
               now - book.received_ns <= limits_.max_book_age_ns;
    }

    void mark() noexcept {
        const auto inventory_value = stats_.position * (book_.bid + (book_.ask - book_.bid) / 2);
        if ((inventory_value > 0 && stats_.cash > INT64_MAX - inventory_value) ||
            (inventory_value < 0 && stats_.cash < INT64_MIN - inventory_value)) {
            kill();
            return;
        }
        stats_.pnl = stats_.cash + inventory_value;
        if (stats_.pnl <= -limits_.max_loss) kill();
    }
};
} // namespace ull
