#pragma once

#include "ull/nasdaq/itch.hpp"
#include <bitset>

namespace ull::nasdaq {

enum class BookError { none, transport, decode, system_event, directory,
                       duplicate_order, unknown_order, excess_reduction, capacity };

struct DisplayOrder {
    std::uint64_t reference = 0;
    std::uint32_t shares = 0;
    std::uint32_t price = 0;
    char side = 0;
    std::array<char, 4> attribution{};
};

struct BookSnapshot {
    Symbol stock{};
    std::uint16_t locate = 0;
    std::uint64_t generation = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint32_t bid = 0, ask = 0;
    std::uint64_t bid_shares = 0, ask_shares = 0;
    std::size_t orders = 0;
    char trading_state = 'H', system_event = 0, reg_sho = '?';
    bool operational_halt = false, circuit_breaker = false, feed_active = false;

    // A conservative displayed-market status, NOT authorization to send orders.
    bool continuous_trading() const noexcept {
        return feed_active && system_event == 'Q' && trading_state == 'T' &&
               !operational_halt && !circuit_breaker;
    }
};

// Single-symbol, day-start replay reference. Fixed-capacity open addressing for
// orders; snapshot/depth reads scan the table. No hot-path allocations. Not a
// constant-time production price-level book or a GLIMPSE recovery implementation.
template <std::size_t Capacity = 4096>
class ItchBook {
    static_assert(Capacity >= 2 && Capacity <= 1'048'576 && (Capacity & (Capacity - 1)) == 0);
public:
    explicit ItchBook(Symbol stock) noexcept : stock_(stock) {}

    // Requires full replay from message 1 including System Event O and directory.
    // This clears all old IDs, locates and halts; there is no mid-session rearm.
    void start_session(Session session) noexcept {
        slots_ = {};
        directory_locates_.reset();
        state_ = {};
        state_.stock = stock_;
        error_ = BookError::none;
        started_ = false;
        receiver_.arm(session, 1);
    }

    bool process(Bytes datagram) noexcept {
        if (error_ != BookError::none) return false;
        const auto result = receiver_.receive(datagram);
        switch (result.status) {
        case FeedStatus::duplicate: case FeedStatus::heartbeat: return true;
        case FeedStatus::end_session: state_.feed_active = false; return true;
        case FeedStatus::data: break;
        default: return fail(BookError::transport);
        }
        bool decoded = true;
        result.packet->visit(result.skip, [&](std::uint64_t, Bytes bytes) noexcept {
            if (!decode_itch(bytes)) decoded = false;
        });
        if (!decoded) return fail(BookError::decode);
        // No snapshots/callbacks escape while the batch is being applied. A later
        // state error invalidates the ENTIRE book until explicit full replay.
        result.packet->visit(result.skip, [&](std::uint64_t, Bytes bytes) noexcept {
            if (error_ == BookError::none) apply(*decode_itch(bytes));
        });
        return error_ == BookError::none;
    }

    [[nodiscard]] BookError error() const noexcept { return error_; }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return receiver_.next_sequence(); }

    [[nodiscard]] std::optional<BookSnapshot> snapshot() const noexcept {
        if (!visible()) return std::nullopt;
        auto result = state_;
        for (const auto& slot : slots_) {
            if (slot.state != SlotState::live) continue;
            const auto& order = slot.order;
            ++result.orders;
            auto& price = order.side == 'B' ? result.bid : result.ask;
            auto& shares = order.side == 'B' ? result.bid_shares : result.ask_shares;
            if (price == 0 || (order.side == 'B' ? order.price > price : order.price < price)) {
                price = order.price;
                shares = order.shares;
            } else if (order.price == price) shares += order.shares;
        }
        return result;
    }

    [[nodiscard]] std::optional<DisplayOrder> order(std::uint64_t reference) const noexcept {
        if (!visible()) return std::nullopt;
        const auto index = find(reference);
        if (index == Capacity) return std::nullopt;
        return slots_[index].order;
    }

    [[nodiscard]] std::optional<std::uint64_t> depth(char side, std::uint32_t price) const noexcept {
        if (!visible() || (side != 'B' && side != 'S')) return std::nullopt;
        std::uint64_t shares = 0;
        for (const auto& slot : slots_)
            if (slot.state == SlotState::live && slot.order.side == side && slot.order.price == price)
                shares += slot.order.shares;
        return shares;
    }

private:
    enum class SlotState : std::uint8_t { empty, live, deleted };
    struct Slot { DisplayOrder order{}; SlotState state = SlotState::empty; };
    Symbol stock_;
    MoldReceiver receiver_;
    std::array<Slot, Capacity> slots_{};
    std::bitset<65'536> directory_locates_{};
    BookSnapshot state_{};
    BookError error_ = BookError::none;
    bool started_ = false;

    bool visible() const noexcept { return error_ == BookError::none && started_ && state_.locate != 0; }
    bool fail(BookError error) noexcept {
        error_ = error;
        state_.feed_active = false;
        receiver_.disarm();
        return false;
    }
    static std::size_t bucket(std::uint64_t id) noexcept {
        id ^= id >> 33;
        id *= 0xff51afd7ed558ccdULL;
        id ^= id >> 33;
        return static_cast<std::size_t>(id) & (Capacity - 1);
    }
    std::size_t find(std::uint64_t id) const noexcept {
        const auto start = bucket(id);
        for (std::size_t n = 0; n < Capacity; ++n) {
            const auto index = (start + n) & (Capacity - 1);
            const auto& slot = slots_[index];
            if (slot.state == SlotState::empty) break;
            if (slot.state == SlotState::live && slot.order.reference == id) return index;
        }
        return Capacity;
    }
    std::size_t free_slot(std::uint64_t id) const noexcept {
        const auto start = bucket(id);
        for (std::size_t n = 0; n < Capacity; ++n) {
            const auto index = (start + n) & (Capacity - 1);
            if (slots_[index].state != SlotState::live) return index;
        }
        return Capacity;
    }
    void changed(const ItchMessage& m) noexcept {
        ++state_.generation;
        state_.timestamp_ns = m.timestamp_ns;
    }

    void apply(const ItchMessage& m) noexcept {
        if (m.type == 'S') {
            const std::string_view events = "OSQMEC";
            const auto previous = events.find(state_.system_event);
            const auto expected = started_ ? previous + 1 : 0;
            if (expected >= events.size() || m.state != events[expected]) {
                fail(BookError::system_event); return;
            }
            started_ = true;
            state_.system_event = m.state;
            state_.feed_active = m.state != 'C';
            changed(m);
            return;
        }
        if (!started_ || state_.system_event == 'C') { fail(BookError::system_event); return; }
        if (m.type == 'W') {
            state_.circuit_breaker = true; // Conservative latch; no implicit resume.
            changed(m); return;
        }
        if (m.type == 'R') {
            if (directory_locates_.test(m.locate)) { fail(BookError::directory); return; }
            directory_locates_.set(m.locate);
            if (m.stock == stock_) {
                if (state_.locate != 0) { fail(BookError::directory); return; }
                state_.locate = m.locate;
                changed(m);
            } else if (state_.locate == m.locate) fail(BookError::directory);
            return;
        }
        // Detect a target-symbol event with a wrong/missing locate even before
        // directory discovery; never silently ignore it as a different stock.
        const bool has_symbol = m.type == 'A' || m.type == 'F' || m.type == 'H' || m.type == 'Y' || m.type == 'h';
        const bool order_event = std::string_view("AFECXDU").find(m.type) != std::string_view::npos;
        if ((has_symbol || order_event) && !directory_locates_.test(m.locate)) {
            fail(BookError::directory); return;
        }
        if (has_symbol && m.stock == stock_ && (state_.locate == 0 || m.locate != state_.locate)) {
            fail(BookError::directory); return;
        }
        if (state_.locate == 0 || m.locate != state_.locate) {
            if (order_event && find(m.reference) != Capacity) fail(BookError::directory);
            return;
        }
        if (has_symbol && m.stock != stock_) { fail(BookError::directory); return; }
        switch (m.type) {
        case 'H': state_.trading_state = m.state; changed(m); return;
        case 'Y': state_.reg_sho = m.state; changed(m); return;
        case 'h':
            if (m.market == 'Q') { state_.operational_halt = m.state == 'H'; changed(m); }
            return;
        case 'A': case 'F': {
            if (find(m.reference) != Capacity) { fail(BookError::duplicate_order); return; }
            const auto index = free_slot(m.reference);
            if (index == Capacity) { fail(BookError::capacity); return; }
            slots_[index] = {{m.reference, m.shares, m.price, m.side, m.attribution}, SlotState::live};
            changed(m); return;
        }
        case 'E': case 'C': case 'X': case 'D': case 'U': break;
        default: return; // P/Q/B do not change the displayed book (including breaks).
        }
        const auto index = find(m.reference);
        if (index == Capacity) { fail(BookError::unknown_order); return; }
        auto& slot = slots_[index];
        if (m.type == 'U') {
            if (find(m.replacement) != Capacity) { fail(BookError::duplicate_order); return; }
            auto replacement = slot.order; // Preserve side and attribution.
            replacement.reference = m.replacement;
            replacement.shares = m.shares;
            replacement.price = m.price;
            slot.state = SlotState::deleted;
            slots_[free_slot(m.replacement)] = {replacement, SlotState::live};
        } else if (m.type == 'D') slot.state = SlotState::deleted;
        else {
            // ITCH E/C/X are DELTAS. "Cumulative effects" in the spec does not
            // mean cumulative execution quantities like the toy engine API.
            if (m.shares > slot.order.shares) { fail(BookError::excess_reduction); return; }
            slot.order.shares -= m.shares;
            if (slot.order.shares == 0) slot.state = SlotState::deleted;
        }
        changed(m);
    }
};
} // namespace ull::nasdaq
