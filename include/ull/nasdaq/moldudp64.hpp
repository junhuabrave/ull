#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace ull::nasdaq {

using Bytes = std::span<const std::uint8_t>;
using Session = std::array<std::uint8_t, 10>;

// Nasdaq MoldUDP64 v1.00. This accepts a UDP PAYLOAD, not an Ethernet frame.
// The caller owns the buffer and must keep it alive and immutable through visit().
class MoldPacket {
public:
    static std::optional<MoldPacket> parse(Bytes bytes) noexcept {
        if (bytes.size() < 20) return std::nullopt;
        const auto count = u16(bytes, 18);
        const auto sequence = u64(bytes, 10);
        const bool control = count == 0 || count == 0xffff;
        // The exclusive next sequence must be representable; never wrap a feed.
        if (!control && sequence > UINT64_MAX - count) return std::nullopt;
        std::size_t offset = 20;
        if (!control) {
            for (std::uint16_t i = 0; i < count; ++i) {
                if (bytes.size() - offset < 2) return std::nullopt;
                const auto length = u16(bytes, offset);
                offset += 2;
                if (length > bytes.size() - offset) return std::nullopt;
                offset += length; // Zero-length messages ARE valid in MoldUDP64.
            }
        }
        if (offset != bytes.size()) return std::nullopt;
        return MoldPacket(bytes, sequence, count);
    }

    [[nodiscard]] Session session() const noexcept {
        Session value{};
        for (std::size_t i = 0; i < value.size(); ++i) value[i] = bytes_[i];
        return value;
    }
    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }
    [[nodiscard]] std::uint16_t count() const noexcept { return count_; }

    // All message boundaries have already been checked, including the last one.
    // This is framing validation only: the next layer must validate ITCH and
    // book semantics before allowing quotes. Callback failure is not rollback.
    template <typename Consumer>
    void visit(std::uint16_t skip, Consumer&& consumer) const noexcept {
        static_assert(std::is_nothrow_invocable_v<Consumer&, std::uint64_t, Bytes>);
        if (count_ == 0 || count_ == 0xffff) return;
        std::size_t offset = 20;
        for (std::uint16_t i = 0; i < count_; ++i) {
            const auto length = u16(bytes_, offset);
            offset += 2;
            if (i >= skip) consumer(sequence_ + i, bytes_.subspan(offset, length));
            offset += length;
        }
    }

private:
    Bytes bytes_;
    std::uint64_t sequence_;
    std::uint16_t count_;

    MoldPacket(Bytes bytes, std::uint64_t sequence, std::uint16_t count) noexcept
        : bytes_(bytes), sequence_(sequence), count_(count) {}

    static std::uint16_t u16(Bytes bytes, std::size_t offset) noexcept {
        return static_cast<std::uint16_t>((std::uint16_t{bytes[offset]} << 8) | bytes[offset + 1]);
    }
    static std::uint64_t u64(Bytes bytes, std::size_t offset) noexcept {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i) value = (value << 8) | bytes[offset + i];
        return value;
    }
};

enum class FeedStatus {
    data, duplicate, heartbeat, end_session, malformed, session_mismatch, gap, disarmed
};

struct FeedResult {
    FeedStatus status;
    std::optional<MoldPacket> packet{}; // Present ONLY for new data.
    std::uint16_t skip = 0; // Suppress an already-applied prefix on overlapping packets.
};

// One owner per feed/session, shared by redundant copies of that same stream.
// Sequence progress is NOT book health or permission to trade. A complete adapter
// must also gate packet integrity, ITCH decoding, book recovery and trading status.
class MoldReceiver {
public:
    // Only after trusted startup/replay checkpoint or completed recovery. Never
    // arm from the first observed multicast packet: it may begin mid-book.
    void arm(Session session, std::uint64_t next) noexcept {
        session_ = session;
        next_ = next;
        ready_ = true;
    }
    void disarm() noexcept { ready_ = false; }
    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_; }

    FeedResult receive(Bytes bytes) noexcept {
        const auto packet = MoldPacket::parse(bytes);
        if (!packet) return stop(FeedStatus::malformed);
        if (!ready_) return {FeedStatus::disarmed};
        if (packet->session() != session_) return stop(FeedStatus::session_mismatch);
        const auto sequence = packet->sequence();
        if (sequence > next_) return stop(FeedStatus::gap);
        const auto count = packet->count();
        if (count == 0 || count == 0xffff) {
            if (sequence < next_) return {FeedStatus::duplicate};
            if (count == 0xffff) return stop(FeedStatus::end_session);
            return {FeedStatus::heartbeat};
        }
        const auto end = sequence + count; // parse() checked overflow.
        if (end <= next_) return {FeedStatus::duplicate};
        const auto skip = static_cast<std::uint16_t>(next_ - sequence);
        next_ = end;
        return {FeedStatus::data, packet, skip};
    }

private:
    Session session_{};
    std::uint64_t next_ = 0;
    bool ready_ = false;
    FeedResult stop(FeedStatus reason) noexcept {
        ready_ = false; // Late packets cannot silently clear a recovery latch.
        return {reason};
    }
};
} // namespace ull::nasdaq
