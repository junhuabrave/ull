#include "ull/nasdaq/ouch.hpp"
#include "ull/nasdaq/soupbintcp.hpp"
#include <cstdlib>
#include <iostream>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { std::cerr << __LINE__ << ": " #expr "\n"; std::abort(); } } while (false)
using namespace ull::nasdaq;
using Client = SoupClient<>;
using View = Client::View;

// Independent wire fixtures from OUCH 5.0 tables; price is 0x01020304 price4.
constexpr OuchEnter order{0x12345678, 'B', 0x010203, "AAPL", 0x01020304, "abc123"};
constexpr std::array<std::uint8_t, 56> entered = {
    'O', 0x12,0x34,0x56,0x78, 'B', 0,1,2,3, 'A','A','P','L',' ',' ',' ',' ',
    0,0,0,0,1,2,3,4, '0','Y','P','N','N',
    'a','b','c','1','2','3',' ',' ',' ',' ',' ',' ',' ',' ',
    0,9, 2,6,'L', 2,12,'P', 2,16,'N'
};
constexpr std::array<std::uint8_t, 33> accepted = {
    0,31,'A', ' ',' ',' ',' ',' ',' ','S','0','0','7',
    ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ','1'
};
constexpr auto ignore = [](std::uint64_t, View) noexcept { return true; };

void encoding() {
    CHECK(OuchEncoder::encode(order) == entered);
    const OuchReplace replace{0x12345678, 0x12345679, 700, 0x05060708, "repl9"};
    const std::array<std::uint8_t, 49> replaced = {
        'U',0x12,0x34,0x56,0x78, 0x12,0x34,0x56,0x79, 0,0,2,0xbc,
        0,0,0,0,5,6,7,8, '0','Y','N',
        'r','e','p','l','9',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        0,9, 2,6,'L', 2,12,'P', 2,16,'N'
    };
    CHECK(OuchEncoder::encode(replace) == replaced);
    const std::array<std::uint8_t, 9> canceled = {'X',0x12,0x34,0x56,0x78,0,0,1,0x90};
    CHECK(OuchEncoder::encode(OuchCancel{order.user_ref, 400}) == canceled);
    const auto full = OuchEncoder::encode(OuchCancel{order.user_ref, 0});
    CHECK(full && (*full)[5] == 0 && (*full)[8] == 0);
    // Bounds exclude market sentinels and never silently truncate or clamp.
    auto o = order;
    for (const auto q : {0U, 1'000'000U, UINT32_MAX}) {
        o.quantity = q; CHECK(!OuchEncoder::encode(o));
        auto r = replace; r.total_quantity = q; CHECK(!OuchEncoder::encode(r));
    }
    o = order; o.quantity = 999'999; CHECK(OuchEncoder::encode(o));
    o.quantity = 1; CHECK(OuchEncoder::encode(o));
    o = order;
    for (const std::uint64_t p : {0ULL, 1'999'999'901ULL, 2'000'000'000ULL, 0x7fffffffULL, 0xffffffffffffffffULL}) {
        o.price4 = p; CHECK(!OuchEncoder::encode(o));
        auto r = replace; r.price4 = p; CHECK(!OuchEncoder::encode(r));
    }
    o.price4 = 1'999'999'900; CHECK(OuchEncoder::encode(o));
    o.price4 = 1; CHECK(OuchEncoder::encode(o));
    o = order; o.side = 'S'; CHECK(OuchEncoder::encode(o)->at(5) == 'S');
    for (const char side : {'T','E','?'}) { o.side = side; CHECK(!OuchEncoder::encode(o)); }
    o = order; o.user_ref = 0; CHECK(!OuchEncoder::encode(o));
    auto r = replace; r.user_ref = r.original_ref; CHECK(!OuchEncoder::encode(r));
    r.user_ref = r.original_ref - 1; CHECK(!OuchEncoder::encode(r));
    r = replace; r.original_ref = 0; CHECK(!OuchEncoder::encode(r));
    for (const auto s : {"", "TOOLONG99", " AAPL", "AAPL ", "AA\nPL"}) {
        o = order; o.symbol = s; CHECK(!OuchEncoder::encode(o));
    }
    o = order; o.symbol = "ABCDEFGH"; o.cl_ord_id = "123456789ABCDE";
    CHECK(OuchEncoder::encode(o));
    for (const auto id : {"", "123456789ABCDEF", "bad\nID", "bad-ID"}) {
        o = order; o.cl_ord_id = id; CHECK(!OuchEncoder::encode(o));
        r = replace; r.cl_ord_id = id; CHECK(!OuchEncoder::encode(r));
    }
    CHECK(!OuchEncoder::encode(OuchCancel{0, 0}));
    CHECK(!OuchEncoder::encode(OuchCancel{1, 1'000'000}));
}

void login_and_output() {
    Client c;
    std::array<std::uint8_t, 59> output{}; output.fill(0xa5);
    CHECK(c.frame_order(entered, output) == 0);
    CHECK(!c.login("1234567", "secret", 0));
    CHECK(!c.login("user", "", 0));
    const auto login = c.login("ab12", "secret", 10);
    const std::array<std::uint8_t, 49> expected = {
        0,47,'L', 'a','b','1','2',' ',' ', 's','e','c','r','e','t',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',
        ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ','1'
    };
    CHECK(login == expected);
    CHECK(!c.login("ab12", "secret", 10));
    CHECK(c.frame_order(entered, output) == 0 && output[0] == 0xa5);
    CHECK(c.receive(accepted, 11, ignore));
    CHECK(c.state() == SoupState::active && c.next_sequence() == 1);
    CHECK(c.frame_order(entered, std::span(output).first(58)) == 0);
    CHECK(output[0] == 0xa5);
    CHECK(c.frame_order({}, output) == 0);
    CHECK(c.frame_order(entered, output) == 59);
    CHECK(output[0] == 0 && output[1] == 57 && output[2] == 'U');
    CHECK(std::equal(entered.begin(), entered.end(), output.begin() + 3));
    // Both length bytes and the uint16 boundary must work, not just tiny orders.
    std::vector<std::uint8_t> large(65534, 0x9a), framed(65537);
    CHECK(c.frame_order(large, framed) == 65537 && framed[0] == 255 && framed[1] == 255);
    CHECK(framed.back() == 0x9a);
    large.push_back(0); CHECK(c.frame_order(large, framed) == 0);
    c.disconnect(); CHECK(c.frame_order(entered, output) == 0);
}

void fragments_and_resume() {
    // Application bytes are deliberately opaque and include NUL and newline.
    const std::vector<std::uint8_t> data = {0,4,'S',0,10,255, 0,1,'H', 0,2,'+',42, 0,3,'S',66,128};
    std::vector<std::uint8_t> stream = {0,2,'+',7};
    stream.insert(stream.end(), accepted.begin(), accepted.end());
    stream.insert(stream.end(), data.begin(), data.end());
    for (std::size_t width = 1; width <= stream.size(); ++width) {
        Client c; CHECK(c.login("u", "p", 0));
        unsigned count = 0;
        auto consume = [&](std::uint64_t seq, View bytes) noexcept {
            CHECK(seq == ++count);
            if (seq == 1) CHECK(bytes.size() == 3 && bytes[0] == 0 && bytes[1] == 10 && bytes[2] == 255);
            else CHECK(seq == 2 && bytes.size() == 2 && bytes[0] == 66 && bytes[1] == 128);
            return true;
        };
        for (std::size_t at = 0; at < stream.size(); at += width)
            CHECK(c.receive(View(stream).subspan(at, std::min(width, stream.size() - at)), at, consume));
        CHECK(count == 2 && c.next_sequence() == 3 && c.state() == SoupState::active);
    }
    // Break the stream at EVERY byte. Only fully applied S packets checkpoint.
    for (std::size_t cut = 0; cut <= data.size(); ++cut) {
        Client c; CHECK(c.login("u", "p", 0)); CHECK(c.receive(accepted, 0, ignore));
        unsigned count = 0;
        auto consume = [&](std::uint64_t seq, View) noexcept { CHECK(seq == ++count); return true; };
        CHECK(c.receive(View(data).first(cut), 1, consume));
        CHECK(count == (cut >= 18 ? 2U : cut >= 6 ? 1U : 0U));
        c.disconnect(); const auto login = c.login("u", "p", 2); CHECK(login);
        CHECK(std::equal(accepted.begin() + 3, accepted.begin() + 13, login->begin() + 19));
        CHECK((*login)[48] == '1' + count);
        auto ack = accepted; ack[32] = static_cast<std::uint8_t>('1' + count);
        CHECK(c.receive(ack, 3, ignore));
        const std::size_t offset = count == 2 ? 18 : count == 1 ? 6 : 0;
        CHECK(c.receive(View(data).subspan(offset), 4, consume));
        CHECK(count == 2 && c.next_sequence() == 3);
    }
    // Reconnect after a partially received login response starts at the prefix.
    Client c; CHECK(c.login("u", "p", 0));
    CHECK(c.receive(View(accepted).first(20), 1, ignore)); c.disconnect();
    CHECK(c.login("u", "p", 2)); CHECK(c.receive(accepted, 3, ignore));
    // A wider inbound frame exercises both bytes of the length prefix.
    std::vector<std::uint8_t> wide(1024, 0xee); wide[0] = 3; wide[1] = 254; wide[2] = 'S';
    CHECK(c.receive(wide, 4, [](std::uint64_t seq, View bytes) noexcept {
        CHECK(seq == 1 && bytes.size() == 1021 && bytes.back() == 0xee); return true;
    }));
    const std::array<std::uint8_t, 3> empty_message = {0,1,'S'};
    for (unsigned i = 1; i < 300; ++i) CHECK(c.receive(empty_message, 4, ignore));
    c.disconnect(); const auto resume = c.login("u", "p", 5); CHECK(resume);
    CHECK((*resume)[45] == ' ' && (*resume)[46] == '3' && (*resume)[47] == '0' && (*resume)[48] == '1');
    auto ack = accepted; ack[30] = '3'; ack[31] = '0'; ack[32] = '1';
    CHECK(c.receive(ack, 6, ignore)); CHECK(c.next_sequence() == 301);
}

void failures_and_clocks() {
    // Invalid login acceptance never silently skips replay or changes session.
    for (const auto seq : {"                   0", "                   2", "18446744073709551616",
                           "                   -", "                  1 ", "                    "}) {
        Client c; CHECK(c.login("u", "p", 0));
        auto bad = accepted; std::copy_n(seq, 20, bad.begin() + 13);
        CHECK(!c.receive(bad, 1, ignore)); CHECK(c.state() == SoupState::fault);
    }
    Client c; CHECK(c.login("u", "p", 0)); CHECK(c.receive(accepted, 1, ignore));
    c.disconnect(); CHECK(c.login("u", "p", 2));
    auto different = accepted; different[12] = '8';
    CHECK(!c.receive(different, 3, ignore));
    c.disconnect(); CHECK(!c.login("u", "p", 4)); // Fault cannot be cleared by socket churn.
    const std::vector<std::vector<std::uint8_t>> invalid = {
        {0,0}, {4,0}, {255,255}, {0,1,'?'}, {0,2,'H',0}, {0,2,'Z',0}, {0,1,'U'},
        std::vector<std::uint8_t>(accepted.begin(), accepted.end())
    };
    for (const auto& frame : invalid) {
        Client s; CHECK(s.login("u", "p", 0)); CHECK(s.receive(accepted, 0, ignore));
        CHECK(!s.receive(frame, 1, ignore)); CHECK(s.state() == SoupState::fault);
        CHECK(s.next_sequence() == 1);
    }
    for (const char code : {'A','S'}) {
        Client s; CHECK(s.login("u", "p", 0));
        const std::array<std::uint8_t, 4> reject = {0,2,'J',static_cast<std::uint8_t>(code)};
        CHECK(s.receive(reject, 1, ignore)); CHECK(s.state() == SoupState::rejected && s.reject_reason() == code);
        s.disconnect(); CHECK(!s.login("u", "p", 2));
    }
    Client early; CHECK(early.login("u", "p", 0));
    const std::array<std::uint8_t, 4> message = {0,2,'S',9};
    CHECK(!early.receive(message, 1, ignore));
    Client app; CHECK(app.login("u", "p", 0)); CHECK(app.receive(accepted, 0, ignore));
    const std::array<std::uint8_t, 8> pair = {0,2,'S',9, 0,2,'S',10};
    unsigned attempts = 0;
    CHECK(!app.receive(pair, 1, [&](std::uint64_t seq, View) noexcept { ++attempts; return seq == 1; }));
    CHECK(attempts == 2 && app.next_sequence() == 2 && app.state() == SoupState::fault);
    CHECK(!app.receive(message, 2, ignore));
    Client ended; CHECK(ended.login("u", "p", 0)); CHECK(ended.receive(accepted, 0, ignore));
    const std::array<std::uint8_t, 3> end = {0,1,'Z'};
    CHECK(ended.receive(end, 1, ignore)); CHECK(ended.state() == SoupState::ended);
    ended.disconnect(); CHECK(!ended.login("u", "p", 2));

    Client timed; CHECK(timed.login("u", "p", 100)); CHECK(timed.receive(accepted, 100, ignore));
    CHECK(!timed.heartbeat_due(1'000'000'100)); CHECK(timed.heartbeat_due(1'000'000'101));
    std::array<std::uint8_t, 59> output{};
    CHECK(timed.frame_order(entered, output) == 59);
    CHECK(timed.heartbeat_due(1'000'000'101)); // Encoding did NOT count as sending.
    CHECK(timed.sent(1'000'000'101)); CHECK(!timed.heartbeat_due(1'000'000'102));
    CHECK(timed.poll(2'000'000'099, 2'000'000'000));
    CHECK(!timed.poll(2'000'000'100, 2'000'000'000)); // TX cannot refresh RX deadline.
    CHECK(timed.state() == SoupState::disconnected && timed.next_sequence() == 1);
    CHECK(timed.login("u", "p", 2'000'000'101));
    CHECK(timed.receive(accepted, 2'000'000'102, ignore));
    CHECK(!timed.poll(2'000'000'101, 2'000'000'000)); // Backwards monotonic clock fails closed.
    CHECK(timed.state() == SoupState::fault);
}

int main() {
    encoding(); login_and_output(); fragments_and_resume(); failures_and_clocks();
    std::cout << "PASS: OUCH golden wire bytes/bounds, Soup fragmentation/reconnect, sequencing/faults, heartbeat/timeouts\n";
}
