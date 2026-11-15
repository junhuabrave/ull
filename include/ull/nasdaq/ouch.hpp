#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ull::nasdaq {

// Deliberately narrow OUCH 5.0 profile: principal, displayed, market-hours DAY,
// continuous limit orders, non-ISO, PostOnly=P, TradeNow=N. No short-sale support.
// Encoding is NOT authorization, allocation of a UserRefNum, or a send operation.
struct OuchEnter {
    std::uint32_t user_ref;
    char side; // B or S (owned shares only); regulatory checks belong to the caller.
    std::uint32_t quantity;
    std::string_view symbol;
    std::uint64_t price4; // USD x 10,000, not ticks.
    std::string_view cl_ord_id;
};

struct OuchReplace {
    std::uint32_t original_ref;
    std::uint32_t user_ref;
    // Total liability of the chain, INCLUDING fills and SMP-decremented shares.
    std::uint32_t total_quantity;
    std::uint64_t price4;
    std::string_view cl_ord_id;
};

struct OuchCancel {
    std::uint32_t user_ref;
    // Intended TOTAL size, not shares to remove. Zero cancels all remaining shares.
    std::uint32_t intended_quantity;
};

class OuchEncoder {
public:
    static constexpr std::uint64_t max_limit_price4 = 1'999'999'900;

    static std::optional<std::array<std::uint8_t, 56>> encode(const OuchEnter& o) noexcept {
        if (o.user_ref == 0 || (o.side != 'B' && o.side != 'S') ||
            !quantity(o.quantity) || !price(o.price4) || !alpha(o.symbol, 8) ||
            !alpha(o.cl_ord_id, 14)) return std::nullopt;
        std::array<std::uint8_t, 56> b{};
        b[0] = 'O'; integer(b, 1, 4, o.user_ref); b[5] = static_cast<std::uint8_t>(o.side);
        integer(b, 6, 4, o.quantity); text(b, 10, 8, o.symbol);
        integer(b, 18, 8, o.price4);
        b[26] = '0'; b[27] = 'Y'; b[28] = 'P'; b[29] = 'N'; b[30] = 'N';
        text(b, 31, 14, o.cl_ord_id); options(b, 45);
        return b;
    }

    static std::optional<std::array<std::uint8_t, 49>> encode(const OuchReplace& o) noexcept {
        if (o.original_ref == 0 || o.user_ref <= o.original_ref ||
            !quantity(o.total_quantity) || !price(o.price4) ||
            !alpha(o.cl_ord_id, 14)) return std::nullopt;
        std::array<std::uint8_t, 49> b{};
        b[0] = 'U'; integer(b, 1, 4, o.original_ref); integer(b, 5, 4, o.user_ref);
        integer(b, 9, 4, o.total_quantity); integer(b, 13, 8, o.price4);
        b[21] = '0'; b[22] = 'Y'; b[23] = 'N';
        text(b, 24, 14, o.cl_ord_id); options(b, 38);
        return b;
    }

    static std::optional<std::array<std::uint8_t, 9>> encode(const OuchCancel& o) noexcept {
        if (o.user_ref == 0 || o.intended_quantity >= 1'000'000) return std::nullopt;
        std::array<std::uint8_t, 9> b{};
        b[0] = 'X'; integer(b, 1, 4, o.user_ref); integer(b, 5, 4, o.intended_quantity);
        return b; // Optional appendage length is omitted for this message.
    }

private:
    static bool quantity(std::uint32_t q) noexcept { return q > 0 && q < 1'000'000; }
    static bool price(std::uint64_t p) noexcept { return p > 0 && p <= max_limit_price4; }
    static bool alpha(std::string_view s, std::size_t width) noexcept {
        if (s.empty() || s.size() > width || s.front() == ' ' || s.back() == ' ') return false;
        for (const char c : s)
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == ' ')) return false;
        return true; // Narrow symbol profile; unsupported symbol spellings reject.
    }
    static void integer(std::span<std::uint8_t> b, std::size_t at,
                        std::size_t width, std::uint64_t value) noexcept {
        for (std::size_t i = width; i > 0; --i) {
            b[at + i - 1] = static_cast<std::uint8_t>(value); value >>= 8;
        }
    }
    static void text(std::span<std::uint8_t> b, std::size_t at,
                     std::size_t width, std::string_view s) noexcept {
        for (std::size_t i = 0; i < width; ++i)
            b[at + i] = i < s.size() ? static_cast<std::uint8_t>(s[i]) : ' ';
    }
    static void options(std::span<std::uint8_t> b, std::size_t at) noexcept {
        // Appendage length includes all TLVs; TLV length excludes its own byte.
        constexpr std::array<std::uint8_t, 11> values = {0,9, 2,6,'L', 2,12,'P', 2,16,'N'};
        for (std::size_t i = 0; i < values.size(); ++i) b[at + i] = values[i];
    }
};
} // namespace ull::nasdaq
