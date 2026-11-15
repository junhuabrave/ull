#pragma once

#include "ull/nasdaq/moldudp64.hpp"
#include <algorithm>
#include <string_view>

namespace ull::nasdaq {

using Symbol = std::array<char, 8>;

// Nasdaq ITCH 5.0: price values are USD * 10,000, quantities are shares.
// Only fields needed by displayed-book reconstruction are materialized. Known
// non-book messages are length/header checked, not fully semantically decoded.
struct ItchMessage {
    char type = 0;
    std::uint16_t locate = 0;
    std::uint64_t timestamp_ns = 0; // Exchange nanoseconds since midnight, not UTC.
    Symbol stock{};
    std::uint64_t reference = 0;
    std::uint64_t replacement = 0;
    std::uint32_t shares = 0;
    std::uint32_t price = 0;
    char side = 0;
    char state = 0;
    char market = 0;
    std::array<char, 4> attribution{'N', 'S', 'D', 'Q'};
};

inline std::optional<Symbol> stock_symbol(std::string_view text) noexcept {
    if (text.empty() || text.size() > 8) return std::nullopt;
    Symbol stock;
    stock.fill(' ');
    bool padding = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == ' ') padding = true;
        else if (padding || c < 33 || c > 126) return std::nullopt;
        stock[i] = static_cast<char>(c);
    }
    if (stock[0] == ' ') return std::nullopt;
    return stock;
}

inline std::optional<ItchMessage> decode_itch(Bytes bytes) noexcept {
    if (bytes.empty()) return std::nullopt;
    const auto type = static_cast<char>(bytes[0]);
    std::size_t length = 0;
    switch (type) {
    case 'S': case 'W': length = 12; break;
    case 'R': length = 39; break;
    case 'H': length = 25; break;
    case 'Y': case 'N': length = 20; break;
    case 'L': length = 26; break;
    case 'V': case 'J': case 'U': length = 35; break;
    case 'K': length = 28; break;
    case 'h': length = 21; break;
    case 'A': case 'C': length = 36; break;
    case 'F': case 'Q': length = 40; break;
    case 'E': length = 31; break;
    case 'X': length = 23; break;
    case 'D': case 'B': length = 19; break;
    case 'P': length = 44; break;
    case 'I': length = 50; break;
    case 'O': length = 48; break;
    default: return std::nullopt;
    }
    if (bytes.size() != length) return std::nullopt;
    const auto number = [&](std::size_t offset, std::size_t width) noexcept {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < width; ++i) value = (value << 8) | bytes[offset + i];
        return value;
    };
    ItchMessage m;
    m.type = type;
    m.locate = static_cast<std::uint16_t>(number(1, 2));
    m.timestamp_ns = number(5, 6);
    if (m.timestamp_ns >= 86'400'000'000'000ULL) return std::nullopt;
    const bool global = type == 'S' || type == 'V' || type == 'W' || type == 'K';
    if (global != (m.locate == 0)) return std::nullopt;
    const auto symbol_at = [&](std::size_t offset) noexcept {
        const auto stock = stock_symbol(std::string_view(reinterpret_cast<const char*>(bytes.data() + offset), 8));
        if (!stock) return false;
        m.stock = *stock;
        return true;
    };
    const auto one_of = [](char value, std::string_view allowed) noexcept {
        return allowed.find(value) != std::string_view::npos;
    };
    switch (type) {
    case 'S':
        m.state = static_cast<char>(bytes[11]);
        if (!one_of(m.state, "OSQMEC")) return std::nullopt;
        break;
    case 'W':
        m.state = static_cast<char>(bytes[11]);
        if (!one_of(m.state, "123")) return std::nullopt;
        break;
    case 'R':
        if (!symbol_at(11) || number(21, 4) == 0) return std::nullopt;
        break;
    case 'H': case 'Y': case 'h':
        if (!symbol_at(11)) return std::nullopt;
        m.state = static_cast<char>(bytes[type == 'h' ? 20 : 19]);
        if (type == 'H' && !one_of(m.state, "HPQT")) return std::nullopt;
        if (type == 'Y' && !one_of(m.state, "012")) return std::nullopt;
        if (type == 'h') {
            m.market = static_cast<char>(bytes[19]);
            if (!one_of(m.market, "QBX") || !one_of(m.state, "HT")) return std::nullopt;
        }
        break;
    case 'A': case 'F':
        m.reference = number(11, 8);
        m.side = static_cast<char>(bytes[19]);
        m.shares = static_cast<std::uint32_t>(number(20, 4));
        m.price = static_cast<std::uint32_t>(number(32, 4));
        if (!symbol_at(24) || !one_of(m.side, "BS")) return std::nullopt;
        if (type == 'F')
            for (std::size_t i = 0; i < 4; ++i) m.attribution[i] = static_cast<char>(bytes[36 + i]);
        break;
    case 'E': case 'C': case 'X':
        m.reference = number(11, 8);
        m.shares = static_cast<std::uint32_t>(number(19, 4));
        if (type == 'C') {
            m.price = static_cast<std::uint32_t>(number(32, 4));
            if (!one_of(static_cast<char>(bytes[31]), "YN")) return std::nullopt;
        }
        break;
    case 'D': m.reference = number(11, 8); break;
    case 'U':
        m.reference = number(11, 8);
        m.replacement = number(19, 8);
        m.shares = static_cast<std::uint32_t>(number(27, 4));
        m.price = static_cast<std::uint32_t>(number(31, 4));
        if (m.replacement == 0 || m.replacement == m.reference) return std::nullopt;
        break;
    default: break; // Known non-book payload; no displayed-order mutation.
    }
    if (one_of(type, "AFECXDU") && m.reference == 0) return std::nullopt;
    if (one_of(type, "AFECXU") && m.shares == 0) return std::nullopt;
    if (one_of(type, "AFCU") && (m.price == 0 || m.price > 2'000'000'000)) return std::nullopt;
    return m;
}
} // namespace ull::nasdaq
