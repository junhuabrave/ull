#include "ull/nasdaq/moldudp64.hpp"
#include <cstdlib>
#include <iostream>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { std::cerr << __LINE__ << ": " #expr "\n"; std::abort(); } } while (false)
using namespace ull::nasdaq;
using S = FeedStatus;

// Hand-written wire bytes, not produced by an encoder under test. Payloads are
// synthetic opaque messages (NOT ITCH). The third message is intentionally empty.
const std::vector<std::uint8_t> golden = {
    'T','E','S','T','0','0','0','0','0','1',
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08, 0x00,0x03,
    0x00,0x03, 'A','B','C', 0x00,0x01, 'Z', 0x00,0x00
};
constexpr Session session = {'T','E','S','T','0','0','0','0','0','1'};
constexpr std::uint64_t sequence = 0x0102030405060708;

std::vector<std::uint8_t> packet_at(std::uint64_t seq, std::uint16_t count = 3) {
    auto packet = golden;
    for (int i = 17; i >= 10; --i) {
        packet[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(seq & 0xff);
        seq >>= 8;
    }
    packet[18] = static_cast<std::uint8_t>(count >> 8);
    packet[19] = static_cast<std::uint8_t>(count & 0xff);
    if (count == 0 || count == 0xffff) packet.resize(20);
    return packet;
}

void framing() {
    const auto packet = MoldPacket::parse(golden);
    CHECK(packet && packet->session() == session);
    CHECK(packet->sequence() == sequence && packet->count() == 3);
    std::size_t count = 0;
    packet->visit(0, [&](std::uint64_t seq, Bytes bytes) noexcept {
        CHECK(seq == sequence + count);
        if (count == 0) CHECK(bytes.size() == 3 && bytes[0] == 'A' && bytes[2] == 'C');
        if (count == 1) CHECK(bytes.size() == 1 && bytes[0] == 'Z');
        if (count == 2) CHECK(bytes.empty());
        ++count;
    });
    CHECK(count == 3);
    for (std::size_t length = 0; length < golden.size(); ++length)
        CHECK(!MoldPacket::parse(Bytes(golden).first(length)));
    auto bad = golden;
    bad.push_back(0); // No ignored trailing bytes.
    CHECK(!MoldPacket::parse(bad));
    bad = golden; bad[27] = 0xff; bad[28] = 0xff; // Malformed final message.
    CHECK(!MoldPacket::parse(bad));
    bad = golden; bad[19] = 2; // Message count must cover entire datagram.
    CHECK(!MoldPacket::parse(bad));
    CHECK(!MoldPacket::parse(packet_at(UINT64_MAX - 2)));
    CHECK(MoldPacket::parse(packet_at(UINT64_MAX - 3)));
    CHECK(MoldPacket::parse(packet_at(UINT64_MAX, 0)));
    auto heartbeat = packet_at(sequence, 0);
    heartbeat.push_back(0);
    CHECK(!MoldPacket::parse(heartbeat));
    auto ending = packet_at(sequence, 0xffff);
    ending.push_back(0);
    CHECK(!MoldPacket::parse(ending));

    // Exercise BOTH bytes of length and count, not only values below 256.
    auto wide = packet_at(sequence, 0);
    wide[18] = 1; wide[19] = 0; // 256 messages.
    wide.push_back(1); wide.push_back(2); // First payload is 258 bytes.
    wide.insert(wide.end(), 258, 0x5a);
    wide.insert(wide.end(), 255 * 2, 0); // Remaining 255 messages are empty.
    const auto large = MoldPacket::parse(wide);
    CHECK(large && large->count() == 256);
    count = 0;
    large->visit(0, [&](std::uint64_t seq, Bytes bytes) noexcept {
        CHECK(seq == sequence + count);
        if (count == 0) CHECK(bytes.size() == 258 && bytes.back() == 0x5a);
        else CHECK(bytes.empty());
        ++count;
    });
    CHECK(count == 256);
}

void sequencing() {
    MoldReceiver feed;
    CHECK(feed.receive(golden).status == S::disarmed);
    feed.arm(session, sequence);
    auto result = feed.receive(golden);
    CHECK(result.status == S::data && result.skip == 0 && result.packet);
    CHECK(feed.next_sequence() == sequence + 3 && feed.ready());
    CHECK(feed.receive(golden).status == S::duplicate);
    auto overlap = packet_at(sequence + 2);
    result = feed.receive(overlap);
    CHECK(result.status == S::data && result.skip == 1);
    std::size_t count = 0;
    result.packet->visit(result.skip, [&](std::uint64_t seq, Bytes bytes) noexcept {
        CHECK(seq == sequence + 3 + count);
        if (count == 0) CHECK(bytes.size() == 1 && bytes[0] == 'Z');
        if (count == 1) CHECK(bytes.empty());
        ++count;
    });
    CHECK(count == 2 && feed.next_sequence() == sequence + 5);
    CHECK(feed.receive(packet_at(sequence + 4, 0)).status == S::duplicate);
    CHECK(feed.receive(packet_at(sequence + 5, 0)).status == S::heartbeat);
    CHECK(feed.next_sequence() == sequence + 5);
    result = feed.receive(packet_at(sequence + 6));
    CHECK(result.status == S::gap && !result.packet && !feed.ready());
    CHECK(feed.next_sequence() == sequence + 5);
    CHECK(feed.receive(packet_at(sequence + 5)).status == S::disarmed);
    feed.arm(session, sequence + 5); // Explicit simulated recovery checkpoint.
    CHECK(feed.receive(packet_at(sequence + 5)).status == S::data);
    CHECK(feed.receive(packet_at(sequence + 8, 0xffff)).status == S::end_session);
    CHECK(!feed.ready());
    CHECK(feed.receive(packet_at(sequence + 8)).status == S::disarmed);
}

void fail_closed() {
    MoldReceiver feed;
    for (const auto count : {std::uint16_t{0}, std::uint16_t{0xffff}}) {
        feed.arm(session, sequence);
        CHECK(feed.receive(packet_at(sequence + 1, count)).status == S::gap);
        CHECK(!feed.ready() && feed.next_sequence() == sequence);
    }
    feed.arm(session, sequence);
    auto changed = golden; changed[9] = '2';
    CHECK(feed.receive(changed).status == S::session_mismatch);
    CHECK(!feed.ready() && feed.next_sequence() == sequence);
    feed.arm(session, sequence);
    auto broken = golden; broken.pop_back();
    const auto result = feed.receive(broken);
    CHECK(result.status == S::malformed && !result.packet);
    CHECK(!feed.ready() && feed.next_sequence() == sequence);
    // Even duplicate packets must be structurally validated before being ignored.
    feed.arm(session, sequence + 3);
    CHECK(feed.receive(broken).status == S::malformed);
    feed.arm(session, sequence);
    feed.disarm();
    CHECK(feed.receive(golden).status == S::disarmed);
}

int main() {
    framing();
    sequencing();
    fail_closed();
    std::cout << "PASS: MoldUDP64 framing, endian/length boundaries, overlap/dedup, gap/session recovery latch\n";
}
