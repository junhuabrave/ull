#include "ull/nasdaq/itch_book.hpp"
#include <cstdlib>
#include <iostream>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { std::cerr << __LINE__ << ": " #expr "\n"; std::abort(); } } while (false)
using namespace ull::nasdaq;
using Message = std::vector<std::uint8_t>;
constexpr Session session = {'T','E','S','T','0','0','0','0','0','1'};
constexpr Symbol stock = {'A','A','P','L',' ',' ',' ',' '};

// Synthetic wire fixtures with offsets/widths from the Nasdaq specification,
// independent of decoder tables. No production encoder is used to generate them.
void put(Message& m, std::size_t offset, std::size_t width, std::uint64_t value) {
    for (std::size_t i = 0; i < width; ++i) {
        m[offset + width - i - 1] = static_cast<std::uint8_t>(value & 255);
        value >>= 8;
    }
}
void symbol(Message& m, std::size_t offset, Symbol value = stock) {
    for (std::size_t i = 0; i < 8; ++i) m[offset + i] = static_cast<std::uint8_t>(value[i]);
}
Message header(char type, std::size_t size, std::uint16_t locate = 0x1234) {
    Message m(size, 0);
    m[0] = static_cast<std::uint8_t>(type);
    put(m, 1, 2, locate);
    put(m, 5, 6, 0x010203040506);
    return m;
}
Message system_event(char code) {
    auto m = header('S', 12, 0); m[11] = static_cast<std::uint8_t>(code); return m;
}
Message directory(std::uint16_t locate = 0x1234, Symbol name = stock) {
    auto m = header('R', 39, locate); symbol(m, 11, name); put(m, 21, 4, 100); return m;
}
Message action(char code, char type = 'H', char market = 'Q') {
    auto m = header(type, type == 'H' ? 25 : type == 'h' ? 21 : 20);
    symbol(m, 11);
    if (type == 'h') { m[19] = static_cast<std::uint8_t>(market); m[20] = static_cast<std::uint8_t>(code); }
    else m[19] = static_cast<std::uint8_t>(code);
    return m;
}
Message add(std::uint64_t id, char side, std::uint32_t shares, std::uint32_t price, char type = 'A') {
    auto m = header(type, type == 'F' ? 40 : 36);
    put(m, 11, 8, id); m[19] = static_cast<std::uint8_t>(side);
    put(m, 20, 4, shares); symbol(m, 24); put(m, 32, 4, price);
    if (type == 'F') { m[36] = 'T'; m[37] = 'E'; m[38] = 'S'; m[39] = 'T'; }
    return m;
}
Message reduce(char type, std::uint64_t id, std::uint32_t shares = 0) {
    auto m = header(type, type == 'D' ? 19 : type == 'X' ? 23 : type == 'E' ? 31 : 36);
    put(m, 11, 8, id);
    if (type != 'D') put(m, 19, 4, shares);
    if (type == 'C') { m[31] = 'N'; put(m, 32, 4, 999'123); } // Execution != display price.
    return m;
}
Message replace(std::uint64_t old, std::uint64_t next, std::uint32_t shares, std::uint32_t price) {
    auto m = header('U', 35);
    put(m, 11, 8, old); put(m, 19, 8, next); put(m, 27, 4, shares); put(m, 31, 4, price);
    return m;
}
Message packet(std::uint64_t seq, std::initializer_list<Message> messages) {
    Message m(session.begin(), session.end()); m.resize(20);
    put(m, 10, 8, seq); put(m, 18, 2, messages.size());
    for (const auto& message : messages) {
        const auto offset = m.size(); m.resize(offset + 2);
        put(m, offset, 2, message.size());
        m.insert(m.end(), message.begin(), message.end());
    }
    return m;
}
struct Stream {
    std::uint64_t next = 1;
    template <std::size_t N>
    bool send(ItchBook<N>& book, std::initializer_list<Message> messages) {
        const auto bytes = packet(next, messages);
        next += messages.size();
        return book.process(bytes);
    }
    template <std::size_t N>
    void start(ItchBook<N>& book) {
        next = 1;
        book.start_session(session);
        CHECK(send(book, {system_event('O'), directory(), system_event('S'), system_event('Q')}));
    }
};

void decoding() {
    // Literal golden A message checks endian widths and offsets independently.
    Message golden = {'A',0x12,0x34,0xab,0xcd,1,2,3,4,5,6,
                      1,2,3,4,5,6,7,8,'S',1,2,3,4,
                      'A','A','P','L',' ',' ',' ',' ',0x07,0x5b,0xcd,0x15};
    const auto decoded = decode_itch(golden);
    CHECK(decoded && decoded->locate == 4660 && decoded->timestamp_ns == 1'108'152'157'446ULL);
    CHECK(decoded->reference == 72'623'859'790'382'856ULL && decoded->shares == 16'909'060);
    CHECK(decoded->price == 123'456'789 && decoded->side == 'S' && decoded->stock == stock);
    std::vector<Message> fixtures = {golden, directory(), system_event('O'), action('T'), action('1', 'Y'),
        action('H', 'h'), add(8, 'B', 7, 123, 'F'), reduce('E', 8, 3), reduce('C', 8, 2),
        reduce('X', 8, 1), reduce('D', 8), replace(8, 12, 11, 123)};
    for (const auto& m : fixtures) {
        CHECK(decode_itch(m));
        for (std::size_t n = 0; n < m.size(); ++n) CHECK(!decode_itch(Bytes(m).first(n)));
        auto extra = m; extra.push_back(0); CHECK(!decode_itch(extra));
    }
    auto bad = golden; bad[19] = 'X'; CHECK(!decode_itch(bad));
    bad = golden; put(bad, 5, 6, 86'400'000'000'000ULL); CHECK(!decode_itch(bad));
    put(bad, 5, 6, 86'399'999'999'999ULL); CHECK(decode_itch(bad));
    bad = golden; put(bad, 32, 4, 2'000'000'001); CHECK(!decode_itch(bad));
    put(bad, 32, 4, 2'000'000'000); CHECK(decode_itch(bad));
    CHECK(!decode_itch(add(1, 'B', 0, 1)));
    CHECK(!decode_itch(replace(1, 1, 2, 3)));
    CHECK(!decode_itch(system_event('Z')));
    CHECK(!decode_itch(action('R')));
    CHECK(!decode_itch(header('?', 12)));
    CHECK(!stock_symbol("TOO-LONG-SYMBOL") && !stock_symbol("AA PL") && !stock_symbol("        "));
}

void displayed_book() {
    ItchBook<16> book(stock); Stream stream; stream.start(book);
    CHECK(book.snapshot() && !book.snapshot()->continuous_trading()); // No H=T yet.
    CHECK(stream.send(book, {action('T'), add(101, 'B', 100, 1'000'000, 'F'),
        add(103, 'B', 70, 1'000'000), add(107, 'B', 41, 990'000), add(109, 'S', 89, 1'010'000)}));
    auto s = *book.snapshot();
    CHECK(s.continuous_trading() && s.bid == 1'000'000 && s.bid_shares == 170);
    CHECK(s.ask == 1'010'000 && s.ask_shares == 89 && s.orders == 4);
    const auto duplicate = packet(5, {action('T'), add(101, 'B', 100, 1'000'000, 'F'),
        add(103, 'B', 70, 1'000'000), add(107, 'B', 41, 990'000), add(109, 'S', 89, 1'010'000)});
    CHECK(book.process(duplicate) && book.snapshot()->generation == s.generation);
    CHECK(stream.send(book, {reduce('E', 101, 13), reduce('E', 101, 17), reduce('C', 101, 7)}));
    CHECK(book.order(101)->shares == 63); // DELTAS; non-printable C still removes shares.
    CHECK(book.order(101)->price == 1'000'000); // Execution price never reprices the order.
    CHECK(book.snapshot()->bid_shares == 133);
    CHECK(stream.send(book, {replace(101, 113, 23, 1'005'000)}));
    CHECK(!book.order(101) && book.order(113)->side == 'B');
    CHECK((book.order(113)->attribution == std::array<char, 4>{'T','E','S','T'}));
    CHECK(book.snapshot()->bid == 1'005'000 && book.snapshot()->bid_shares == 23);
    CHECK(book.depth('B', 1'000'000) == 70 && book.depth('B', 990'000) == 41);
    CHECK(stream.send(book, {reduce('X', 113, 23), reduce('D', 103)}));
    CHECK(book.snapshot()->bid == 990'000 && book.snapshot()->bid_shares == 41);
    s = *book.snapshot();
    CHECK(stream.send(book, {header('P', 44), header('Q', 40), header('B', 19)}));
    CHECK(book.snapshot()->generation == s.generation && book.snapshot()->orders == 2);
    CHECK(stream.send(book, {reduce('E', 109, 89), reduce('D', 107)}));
    CHECK(book.snapshot()->orders == 0 && book.snapshot()->bid == 0 && book.snapshot()->ask == 0);
    CHECK(book.next_sequence() == stream.next);
}

void trading_states_and_filtering() {
    ItchBook<8> book(stock); Stream stream; stream.start(book);
    CHECK(stream.send(book, {action('T'), action('1', 'Y'), add(1, 'B', 13, 101)}));
    const auto generation = book.snapshot()->generation;
    constexpr Symbol other = {'M','S','F','T',' ',' ',' ',' '};
    auto unrelated = add(500, 'S', 1000, 1); put(unrelated, 1, 2, 7); symbol(unrelated, 24, other);
    auto other_fill = reduce('E', 500, 3); put(other_fill, 1, 2, 7);
    CHECK(stream.send(book, {directory(7, other), unrelated, other_fill}));
    CHECK(book.snapshot()->generation == generation && book.snapshot()->orders == 1);
    CHECK(book.next_sequence() == stream.next); // Channel sequence includes other stocks.
    CHECK(book.snapshot()->reg_sho == '1');
    CHECK(stream.send(book, {action('H')})); CHECK(!book.snapshot()->continuous_trading());
    CHECK(stream.send(book, {action('Q')})); CHECK(!book.snapshot()->continuous_trading());
    CHECK(stream.send(book, {action('T'), action('H', 'h', 'B')}));
    CHECK(book.snapshot()->continuous_trading()); // BX halt cannot halt Nasdaq.
    CHECK(stream.send(book, {action('H', 'h')})); CHECK(!book.snapshot()->continuous_trading());
    CHECK(stream.send(book, {action('T', 'h')})); CHECK(book.snapshot()->continuous_trading());
    auto breaker = header('W', 12, 0); breaker[11] = '2';
    CHECK(stream.send(book, {breaker, action('T')}));
    CHECK(book.snapshot()->circuit_breaker && !book.snapshot()->continuous_trading());
    CHECK(stream.send(book, {system_event('M'), system_event('E'), reduce('D', 1), system_event('C')}));
    CHECK(!book.snapshot()->feed_active && book.snapshot()->orders == 0);
    stream.start(book); // Same table storage, completely new day-state.
    CHECK(book.snapshot()->orders == 0 && !book.snapshot()->circuit_breaker && book.snapshot()->reg_sho == '?');
}

void failures_and_capacity() {
    ItchBook<2> book(stock); Stream stream; stream.start(book);
    CHECK(stream.send(book, {add(1, 'B', 9, 100), add(2, 'S', 7, 103)}));
    CHECK(stream.send(book, {replace(1, 3, 11, 101)})); // Replace must work at full capacity.
    CHECK(book.snapshot()->orders == 2 && book.order(3)->shares == 11);
    CHECK(stream.send(book, {reduce('D', 2), add(4, 'S', 5, 104)})); // Tombstone reuse.
    CHECK(!stream.send(book, {add(5, 'B', 3, 99)}));
    CHECK(book.error() == BookError::capacity && !book.snapshot() && !book.order(3));
    CHECK(!book.depth('B', 101));
    stream.start(book);
    CHECK(stream.send(book, {add(1, 'B', 9, 100)}));
    CHECK(!stream.send(book, {reduce('E', 1, 10)}));
    CHECK(book.error() == BookError::excess_reduction && !book.snapshot());
    CHECK(!stream.send(book, {action('T')})); // Status messages never clear faults.
    stream.start(book);
    CHECK(!stream.send(book, {add(1, 'B', 9, 100), add(1, 'B', 4, 99)}));
    CHECK(book.error() == BookError::duplicate_order && !book.snapshot());
    stream.start(book);
    CHECK(!stream.send(book, {add(1, 'B', 9, 100), reduce('D', 2)}));
    CHECK(book.error() == BookError::unknown_order && !book.snapshot()); // No partial-batch quote.
    stream.start(book);
    CHECK(stream.send(book, {add(1, 'B', 9, 100), add(2, 'S', 7, 103)}));
    CHECK(!stream.send(book, {replace(1, 2, 9, 101)}));
    CHECK(book.error() == BookError::duplicate_order);
    stream.start(book);
    auto malformed = add(2, 'S', 7, 103); malformed.pop_back();
    CHECK(!stream.send(book, {add(1, 'B', 9, 100), malformed}));
    CHECK(book.error() == BookError::decode && !book.snapshot());
    stream.start(book);
    CHECK(!book.process(packet(stream.next + 1, {action('T')})));
    CHECK(book.error() == BookError::transport && !book.snapshot());
    stream.start(book);
    CHECK(!stream.send(book, {directory(77)})); // Intraday remapping is not permitted.
    CHECK(book.error() == BookError::directory);
    stream.start(book);
    auto wrong = add(1, 'B', 9, 100); put(wrong, 1, 2, 77);
    CHECK(!stream.send(book, {wrong}) && book.error() == BookError::directory);
    book.start_session(session);
    CHECK(!book.process(packet(1, {directory()})) && book.error() == BookError::system_event);
    book.start_session(session);
    CHECK(!book.process(packet(1, {system_event('O'), add(1, 'B', 9, 100)})));
    CHECK(book.error() == BookError::directory);
    constexpr Symbol other = {'M','S','F','T',' ',' ',' ',' '};
    book.start_session(session);
    CHECK(!book.process(packet(1, {system_event('O'), directory(0x1234, other), directory()})));
    CHECK(book.error() == BookError::directory); // Locate collision before target discovery.
    stream.start(book);
    CHECK(stream.send(book, {directory(7, other), add(1, 'B', 9, 100)}));
    auto misplaced = reduce('E', 1, 2); put(misplaced, 1, 2, 7);
    CHECK(!stream.send(book, {misplaced}) && book.error() == BookError::directory);

    ItchBook<8> churn(stock); Stream churn_stream; churn_stream.start(churn);
    for (std::uint64_t id = 1; id <= 8; ++id)
        CHECK(churn_stream.send(churn, {add(id, 'B', UINT32_MAX, 101)}));
    for (std::uint64_t id = 1; id <= 80; ++id) {
        CHECK(churn_stream.send(churn, {replace(id, id + 8, UINT32_MAX, 101)}));
        CHECK(!churn.order(id) && churn.order(id + 8)->shares == UINT32_MAX);
        CHECK(churn.snapshot()->orders == 8 && churn.snapshot()->bid_shares == 34'359'738'360ULL);
    }
    auto ending = packet(churn_stream.next, {}); ending[18] = 255; ending[19] = 255;
    CHECK(churn.process(ending));
    CHECK(churn.snapshot() && !churn.snapshot()->feed_active && !churn.snapshot()->continuous_trading());
}

int main() {
    decoding();
    displayed_book();
    trading_states_and_filtering();
    failures_and_capacity();
    std::cout << "PASS: ITCH wire decoding, displayed depth, delta executions, replacements, trading gates, fail-closed replay\n";
}
