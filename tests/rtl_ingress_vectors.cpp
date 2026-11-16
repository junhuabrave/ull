#include "ull/nasdaq/itch.hpp"
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

using namespace ull::nasdaq;
using Wire = std::vector<std::uint8_t>;
constexpr Session session = {'T','E','S','T','0','0','0','0','0','1'};
constexpr std::size_t max_packet = 1024, max_events = 4; // Testbench bounds.

void put(Wire& b, std::size_t at, std::size_t width, std::uint64_t value) {
    for (std::size_t i = width; i > 0; --i) { b.at(at + i - 1) = static_cast<std::uint8_t>(value); value >>= 8; }
}
void stock(Wire& b, std::size_t at) {
    constexpr char symbol[] = "AAPL    ";
    for (std::size_t i = 0; i < 8; ++i) b.at(at + i) = static_cast<std::uint8_t>(symbol[i]);
}
Wire fixture(char type) {
    // Independent fixture sizes and asymmetric field values, not RTL tables.
    std::size_t size = 0;
    switch (type) {
    case 'S': case 'W': size = 12; break;
    case 'R': size = 39; break;
    case 'H': size = 25; break;
    case 'Y': case 'N': size = 20; break;
    case 'h': size = 21; break;
    case 'A': case 'C': size = 36; break;
    case 'F': case 'Q': size = 40; break;
    case 'E': size = 31; break;
    case 'X': size = 23; break;
    case 'D': case 'B': size = 19; break;
    case 'U': case 'V': case 'J': size = 35; break;
    case 'L': size = 26; break;
    case 'K': size = 28; break;
    case 'P': size = 44; break;
    case 'I': size = 50; break;
    case 'O': size = 48; break;
    default: std::abort();
    }
    Wire b(size);
    b[0] = static_cast<std::uint8_t>(type);
    put(b, 1, 2, std::string_view("SVWK").find(type) != std::string_view::npos ? 0 : 0x1234);
    put(b, 3, 2, 0xabcd); put(b, 5, 6, 0x010203040506);
    if (type == 'S') b[11] = 'Q';
    if (type == 'W') b[11] = '2';
    if (std::string_view("RHYh").find(type) != std::string_view::npos) {
        stock(b, 11);
        if (type == 'R') put(b, 21, 4, 100);
        if (type == 'H') b[19] = 'T';
        if (type == 'Y') b[19] = '1';
        if (type == 'h') { b[19] = 'Q'; b[20] = 'H'; }
    }
    if (std::string_view("AFECXDU").find(type) != std::string_view::npos) put(b, 11, 8, 0x0102030405060708);
    if (type == 'A' || type == 'F') {
        b[19] = 'S'; put(b, 20, 4, 0x01020304); stock(b, 24); put(b, 32, 4, 123456789);
        if (type == 'F') { b[36] = 'T'; b[37] = 'E'; b[38] = 'S'; b[39] = 'T'; }
    }
    if (std::string_view("ECX").find(type) != std::string_view::npos) put(b, 19, 4, 0x11223344);
    if (type == 'C') { b[31] = 'N'; put(b, 32, 4, 987654321); }
    if (type == 'U') { put(b, 19, 8, 0x1112131415161718); put(b, 27, 4, 0x12345678); put(b, 31, 4, 7654321); }
    if (!decode_itch(b)) std::abort();
    return b;
}
Wire packet(std::uint64_t sequence, const std::vector<Wire>& messages) {
    Wire b(session.begin(), session.end()); b.resize(20);
    put(b, 10, 8, sequence); put(b, 18, 2, messages.size());
    for (const auto& message : messages) {
        const auto at = b.size(); b.resize(at + 2); put(b, at, 2, message.size());
        b.insert(b.end(), message.begin(), message.end());
    }
    return b;
}
std::string event_hex(std::uint64_t sequence, const ItchMessage& m) {
    std::ostringstream out; out << std::hex << std::setfill('0');
    auto n = [&](std::uint64_t value, int digits) { out << std::setw(digits) << value; };
    n(sequence,16); n(static_cast<unsigned char>(m.type),2); n(m.locate,4); n(m.timestamp_ns,12);
    for (const auto c : m.stock) n(static_cast<unsigned char>(c),2);
    n(m.reference,16); n(m.replacement,16); n(m.shares,8); n(m.price,8);
    n(static_cast<unsigned char>(m.side),2); n(static_cast<unsigned char>(m.state),2);
    n(static_cast<unsigned char>(m.market),2);
    for (const auto c : m.attribution) n(static_cast<unsigned char>(c),2);
    return out.str();
}

struct Corpus {
    MoldReceiver model;
    unsigned cases = 0, emitted = 0;
    void arm(std::uint64_t next = 1) {
        std::cout << "A 54455354303030303031 " << std::hex << next << std::dec << '\n';
        model.arm(session, next);
    }
    void reset() { std::cout << "R\n"; model = MoldReceiver{}; }
    void send(const Wire& b, int keep_fault = 0, bool integrity = true, int abort_output = 0) {
        auto candidate = model;
        std::vector<std::string> events;
        int status;
        if (b.size() > max_packet || keep_fault || !MoldPacket::parse(b)) status = 4;
        else if (!integrity) status = 10;
        else {
            const auto result = candidate.receive(b);
            status = static_cast<int>(result.status);
            if (result.packet) {
                bool valid = true;
                result.packet->visit(result.skip, [&](std::uint64_t seq, Bytes bytes) noexcept {
                    const auto m = decode_itch(bytes);
                    if (m) events.push_back(event_hex(seq, *m));
                    else valid = false;
                });
                if (!valid) status = 8;
                else if (events.size() > max_events) status = 9;
            }
        }
        // Transactional wrapper: unlike transport-only MoldReceiver, publish the
        // new checkpoint only after all ITCH messages AND the hardware bounds pass.
        if (status == 0 || status == 1 || status == 2) model = candidate;
        else { model.disarm(); events.clear(); }
        if (status != 0) events.clear();
        std::cout << "P " << b.size() << ' ' << keep_fault << ' ' << integrity << ' ' << abort_output << ' '
                  << status << ' ' << std::hex << model.next_sequence() << std::dec << ' '
                  << model.ready() << ' ' << events.size() << '\n';
        for (const auto byte : b) std::cout << std::hex << static_cast<unsigned>(byte) << ' ';
        std::cout << std::dec << '\n';
        for (const auto& event : events) std::cout << event << '\n';
        ++cases; emitted += static_cast<unsigned>(events.size());
        if (abort_output == 1) model = MoldReceiver{};
        if (abort_output == 2) model.arm(session, 7);
    }
    void isolated(const Wire& b) { arm(); send(b); }
};

int main() {
    Corpus c;
    const auto add = fixture('A'), fill = fixture('C'), sys = fixture('S');
    const auto pair = packet(1, {add, fill});
    c.send(pair); // Disarmed after reset.
    c.arm(); c.send(pair); c.send(pair); // Exact duplicate.
    c.send(packet(2, {fill, fixture('U')})); // Overlap: only sequence 3 is new.
    c.send(packet(4, {})); c.send(packet(3, {})); // Current/stale heartbeat.
    auto end = packet(4, {}); put(end,18,2,65535); c.send(end); c.send(pair);
    c.arm(); c.send(packet(2,{add})); c.send(pair); // Gap remains latched.
    auto mismatch = pair; mismatch[9] = '2'; c.isolated(mismatch);
    for (const auto count : {0U,1U,3U}) {
        auto wrong_count = pair; put(wrong_count,18,2,count); c.isolated(wrong_count);
    }
    c.arm(); c.send(packet(2,{})); // Forward heartbeat is also a gap.
    c.arm(5); c.send(end); // Stale end marker must not disarm.
    c.arm(); c.send(pair,0,false); c.send(pair); // Integrity failure retains checkpoint.
    for (int mode : {1,2,3}) { c.arm(); c.send(pair, mode); }
    c.arm(); c.send(pair,0,true,1); c.send(pair); // Reset while output stalled.
    c.arm(); c.send(pair,0,true,2); c.send(packet(7,{fill})); // Rearm while stalled.
    std::cout << "C\n"; c.reset(); c.arm(); c.send(pair); // Reset during partial header.
    std::cout << "C\n"; c.arm(); c.send(pair); // Rearm during partial header.

    for (const char t : std::string_view("SRHYhWAFECXDULVKJPQBINO")) {
        const auto m = fixture(t);
        c.isolated(packet(1,{m}));
        // Every truncation of a Mold+ITCH packet, including split length/header.
        const auto p = packet(1,{m});
        for (std::size_t n = 0; n < p.size(); ++n) c.isolated(Wire(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(n)));
        auto extra = p; extra.push_back(0); c.isolated(extra);
        // Aligned/padded raw ITCH lengths are still rejected by exact length checks.
        auto longer = m; longer.push_back(0); c.isolated(packet(1,{longer}));
        // Mutate every byte. Valid mutations must emit changed fields, not just
        // avoid a crash. Reference payload validation intentionally varies by type.
        for (std::size_t at = 0; at < m.size(); ++at) {
            for (const auto mask : {0x01,0x80,0xff}) {
                auto mutated = m; mutated[at] ^= static_cast<std::uint8_t>(mask);
                c.isolated(packet(1,{add,mutated})); // Invalid final message must suppress the first too.
            }
        }
    }
    for (const std::uint64_t stamp : {86'399'999'999'999ULL,86'400'000'000'000ULL}) {
        auto m = add; put(m,5,6,stamp); c.isolated(packet(1,{m}));
    }
    for (const auto price : {0U,2'000'000'000U,2'000'000'001U}) {
        auto m = add; put(m,32,4,price); c.isolated(packet(1,{m}));
    }
    auto zero_id = add; put(zero_id,11,8,0); c.isolated(packet(1,{zero_id}));
    auto zero_size = add; put(zero_size,20,4,0); c.isolated(packet(1,{zero_size}));
    for (const char ch : {' ', '\0', '\x7f'}) {
        auto m = add; m[24] = static_cast<std::uint8_t>(ch); c.isolated(packet(1,{m}));
    }
    c.isolated(packet(1,{{}})); // Mold zero length valid, new ITCH zero length invalid.
    c.arm(2); c.send(packet(1,{{},add})); // Empty duplicate prefix must be skipped.
    c.arm(3); c.send(packet(1,{{},{}}));
    auto broken = pair; broken.pop_back(); c.arm(3); c.send(broken); // Duplicates still require valid framing.
    c.isolated(packet(1,{sys,sys,sys,sys})); // Exact event capacity.
    c.isolated(packet(1,{sys,sys,sys,sys,sys}));
    c.arm(257); c.send(packet(1,std::vector<Wire>(256))); // Both count bytes; duplicate empty blocks.
    for (const std::size_t length : {1001U,1002U,1003U}) {
        c.arm(2); c.send(packet(1,{Wire(length,0xab)})); // Packet limit -1 / exact / +1, wide lengths.
    }
    c.arm(UINT64_MAX - 1); c.send(packet(UINT64_MAX - 1,{add}));
    c.send(packet(UINT64_MAX,{})); c.send(packet(UINT64_MAX,{add})); // Exclusive sequence overflow.
    c.arm(UINT64_MAX); auto last = packet(UINT64_MAX,{}); put(last,18,2,65535); c.send(last);
    // Mixed valid messages cover all eight message-boundary lanes, and continuous
    // packets exercise sequence progress with no rearm between packets.
    c.arm();
    std::uint64_t rng = 0x123456789abcdef, next = 1;
    constexpr std::string_view kinds = "AFECXDUSRHYhW";
    for (unsigned i = 0; i < 200; ++i) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        const auto count = 1 + rng % 4;
        std::vector<Wire> messages;
        for (unsigned j = 0; j < count; ++j) messages.push_back(fixture(kinds[(i + j) % kinds.size()]));
        c.send(packet(next,messages)); next += count;
    }
    std::cout << "Z " << c.cases << ' ' << c.emitted << '\n';
    std::cerr << "Generated " << c.cases << " packet cases and " << c.emitted << " expected events\n";
}
