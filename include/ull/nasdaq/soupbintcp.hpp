#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace ull::nasdaq {

enum class SoupState { disconnected, login_pending, active, rejected, ended, fault };

// Offline SoupBinTCP 3.00 client. Owns framing + inbound sequencing, NOT TCP,
// order/exposure state, retransmission of requests, or durable checkpoints.
// Single owner; callbacks must not reenter this object or retain borrowed bytes.
template <std::size_t MaxPacket = 1024>
class SoupClient {
    static_assert(MaxPacket >= 33 && MaxPacket <= 65537);
public:
    using View = std::span<const std::uint8_t>;
    using Login = std::array<std::uint8_t, 49>;
    static constexpr std::array<std::uint8_t, 3> heartbeat = {0,1,'R'};
    static constexpr std::array<std::uint8_t, 3> logout = {0,1,'O'};

    // First connection requests the active session from sequence 1. Reconnect
    // uses ONLY the session and next sequence retained by this same instance.
    // No "latest" (sequence 0) shortcut or unverified checkpoint import.
    std::optional<Login> login(std::string_view user, std::string_view password,
                               std::uint64_t now_ns) noexcept {
        if (state_ != SoupState::disconnected || !credential(user, 6) ||
            !credential(password, 10)) return std::nullopt;
        if (!clock(now_ns)) return std::nullopt;
        Login b{}; b.fill(' '); b[0] = 0; b[1] = 47; b[2] = 'L';
        std::copy(user.begin(), user.end(), b.begin() + 3);
        std::copy(password.begin(), password.end(), b.begin() + 9);
        std::copy(session_.begin(), session_.end(), b.begin() + 19);
        auto n = next_;
        for (std::size_t i = 49; n != 0; n /= 10) b[--i] = static_cast<std::uint8_t>('0' + n % 10);
        used_ = 0; needed_ = 2;
        last_rx_ = last_tx_ = now_ns; // Timer origin, NOT evidence of socket transmission.
        state_ = SoupState::login_pending;
        return b; // Contains credentials: caller must not log this frame.
    }

    // Called on socket loss/timeout. Never releases order reservations. Incomplete
    // packet bytes are discarded; fully consumed messages remain checkpointed.
    void disconnect() noexcept {
        used_ = 0; needed_ = 2;
        if (state_ == SoupState::active || state_ == SoupState::login_pending)
            state_ = SoupState::disconnected;
        // Faults, rejection and end-of-session cannot be cleared by reconnecting.
    }

    // Handles arbitrary TCP fragmentation/coalescing, including split lengths.
    // Consume returns true ONLY after validating/applying the application message.
    // False latches fault, does not advance sequence, and requires reconciliation;
    // earlier messages in this call are NOT rolled back. No malformed bytes skip.
    template <typename Consumer>
    bool receive(View chunk, std::uint64_t now_ns, Consumer&& consume) noexcept {
        static_assert(std::is_nothrow_invocable_r_v<bool, Consumer&, std::uint64_t, View>);
        if (!connected() || !clock(now_ns)) return false;
        if (!chunk.empty()) last_rx_ = now_ns;
        while (!chunk.empty()) {
            const auto n = std::min(needed_ - used_, chunk.size());
            std::copy_n(chunk.begin(), n, buffer_.begin() + static_cast<std::ptrdiff_t>(used_));
            used_ += n; chunk = chunk.subspan(n);
            if (used_ != needed_) continue;
            if (needed_ == 2) {
                const auto length = (std::size_t{buffer_[0]} << 8) | buffer_[1];
                if (length == 0 || length + 2 > MaxPacket) return fail();
                needed_ = length + 2;
                continue;
            }
            if (!packet(consume)) return false;
            used_ = 0; needed_ = 2;
            if (!connected()) return chunk.empty() ? true : fail();
        }
        return true;
    }

    // Transport readiness only. Caller must independently gate risk/recovery and
    // retain the encoded request unchanged across partial writes/uncertain sends.
    // Source and destination must not overlap. Zero means no frame was produced.
    std::size_t frame_order(View message, std::span<std::uint8_t> output) const noexcept {
        if (state_ != SoupState::active || message.empty() || message.size() > 65534 ||
            output.size() < message.size() + 3) return 0;
        const auto length = message.size() + 1;
        output[0] = static_cast<std::uint8_t>(length >> 8);
        output[1] = static_cast<std::uint8_t>(length); output[2] = 'U';
        std::copy(message.begin(), message.end(), output.begin() + 3);
        return message.size() + 3;
    }

    // Call only after actual positive socket-write progress, NOT after encoding.
    bool sent(std::uint64_t now_ns) noexcept {
        if (!connected() || !clock(now_ns)) return false;
        last_tx_ = now_ns; return true;
    }
    [[nodiscard]] bool heartbeat_due(std::uint64_t now_ns) const noexcept {
        return state_ == SoupState::active && now_ns >= last_tx_ && now_ns - last_tx_ > 1'000'000'000;
    }
    // Caller chooses the receive timeout and calls even when no bytes arrive.
    // Soup 3.00 does not mandate a client receive timeout. Zero is invalid here.
    bool poll(std::uint64_t now_ns, std::uint64_t receive_timeout_ns) noexcept {
        if (!connected()) return false;
        if (!clock(now_ns) || receive_timeout_ns == 0) return fail();
        if (now_ns - last_rx_ >= receive_timeout_ns) { disconnect(); return false; }
        return true;
    }
    [[nodiscard]] SoupState state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_; }
    [[nodiscard]] const std::array<std::uint8_t, 10>& session() const noexcept { return session_; }
    [[nodiscard]] char reject_reason() const noexcept { return reject_reason_; }

private:
    SoupState state_ = SoupState::disconnected;
    std::array<std::uint8_t, MaxPacket> buffer_{};
    std::array<std::uint8_t, 10> session_ = {' ',' ',' ',' ',' ',' ',' ',' ',' ',' '};
    std::size_t used_ = 0, needed_ = 2;
    std::uint64_t next_ = 1, last_rx_ = 0, last_tx_ = 0, now_ = 0;
    bool have_session_ = false;
    char reject_reason_ = 0;

    [[nodiscard]] bool connected() const noexcept {
        return state_ == SoupState::login_pending || state_ == SoupState::active;
    }
    bool fail() noexcept { state_ = SoupState::fault; return false; }
    bool clock(std::uint64_t now_ns) noexcept {
        if (now_ns < now_) return fail();
        now_ = now_ns; return true;
    }
    static bool credential(std::string_view s, std::size_t width) noexcept {
        if (s.empty() || s.size() > width) return false;
        for (const auto c : s) if (c < '!' || c > '~') return false;
        return true;
    }
    static std::optional<std::uint64_t> decimal(View s) noexcept {
        std::uint64_t value = 0;
        std::size_t i = 0;
        while (i < s.size() && s[i] == ' ') ++i;
        if (i == s.size()) return std::nullopt;
        for (; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') return std::nullopt;
            const auto digit = static_cast<std::uint64_t>(s[i] - '0');
            if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
            value = value * 10 + digit;
        }
        return value;
    }
    template <typename Consumer>
    bool packet(Consumer& consume) noexcept {
        const View b(buffer_.data(), needed_);
        const auto type = b[2];
        if (type == '+') return true;
        if (state_ == SoupState::login_pending) {
            if (type == 'J' && b.size() == 4 && (b[3] == 'A' || b[3] == 'S')) {
                reject_reason_ = static_cast<char>(b[3]); state_ = SoupState::rejected; return true;
            }
            if (type != 'A' || b.size() != 33) return fail();
            const auto sequence = decimal(b.subspan(13, 20));
            if (!sequence || *sequence != next_) return fail();
            std::array<std::uint8_t, 10> session{};
            std::copy_n(b.begin() + 3, 10, session.begin());
            bool nonblank = false;
            for (const auto c : session) {
                if (c < ' ' || c > '~') return fail();
                nonblank |= c != ' ';
            }
            if (!nonblank || (have_session_ && session != session_)) return fail();
            session_ = session; have_session_ = true; state_ = SoupState::active; return true;
        }
        if (type == 'H' && b.size() == 3) return true;
        if (type == 'Z' && b.size() == 3) { state_ = SoupState::ended; return true; }
        if (type != 'S' || next_ == UINT64_MAX) return fail();
        if (!consume(next_, b.subspan(3))) return fail();
        ++next_;
        return true;
    }
};
} // namespace ull::nasdaq
