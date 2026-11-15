#include "ull/nasdaq/itch_book.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 4 || std::string_view(argv[1]).size() != 10) {
        std::cerr << "Usage: nasdaq_ingress SESSION10 NEXT_SEQUENCE [--stock SYMBOL] udp-payload.bin [...]\n"
                  << "Offline transport or single-stock ITCH book replay; never sends orders.\n";
        return 2;
    }
    const std::string_view sequence_text(argv[2]);
    std::uint64_t next = 0;
    const auto [end, error] = std::from_chars(sequence_text.data(), sequence_text.data() + sequence_text.size(), next);
    if (error != std::errc{} || end != sequence_text.data() + sequence_text.size()) return 2;
    ull::nasdaq::Session session;
    for (std::size_t i = 0; i < session.size(); ++i) session[i] = static_cast<std::uint8_t>(argv[1][i]);
    std::optional<ull::nasdaq::ItchBook<>> book;
    int first_file = 3;
    if (std::string_view(argv[3]) == "--stock") {
        if (argc < 6 || next != 1) {
            std::cerr << "Book replay requires --stock SYMBOL, input files and NEXT_SEQUENCE=1\n";
            return 2;
        }
        const auto stock = ull::nasdaq::stock_symbol(argv[4]);
        if (!stock) { std::cerr << "Invalid stock symbol\n"; return 2; }
        book.emplace(*stock);
        book->start_session(session);
        first_file = 5;
    }
    ull::nasdaq::MoldReceiver receiver;
    receiver.arm(session, next);
    std::uint64_t messages = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t duplicates = 0;
    for (int i = first_file; i < argc; ++i) {
        std::ifstream input(argv[i], std::ios::binary);
        if (!input) { std::cerr << "Cannot open " << argv[i] << '\n'; return 2; }
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        if (input.bad()) { std::cerr << "Read failed: " << argv[i] << '\n'; return 2; }
        if (book) {
            if (!book->process(bytes)) {
                std::cerr << "Book invalidated by " << argv[i] << ": error=" << static_cast<int>(book->error())
                          << "; full day-start replay required\n";
                return 1;
            }
            continue;
        }
        const auto result = receiver.receive(bytes);
        using S = ull::nasdaq::FeedStatus;
        switch (result.status) {
        case S::data:
            result.packet->visit(result.skip, [&](std::uint64_t, ull::nasdaq::Bytes message) noexcept {
                ++messages;
                payload_bytes += message.size();
            });
            break;
        case S::duplicate: ++duplicates; break;
        case S::heartbeat: break;
        case S::end_session: break;
        default:
            std::cerr << "Feed rejected " << argv[i] << ": status=" << static_cast<int>(result.status)
                      << " expected_sequence=" << receiver.next_sequence() << "; recovery required\n";
            return 1;
        }
    }
    if (book) {
        const auto snapshot = book->snapshot();
        if (!snapshot) { std::cerr << "Selected stock directory not found\n"; return 1; }
        std::cout << "locate=" << snapshot->locate << " generation=" << snapshot->generation
                  << " bid_price4=" << snapshot->bid << " bid_shares=" << snapshot->bid_shares
                  << " ask_price4=" << snapshot->ask << " ask_shares=" << snapshot->ask_shares
                  << " orders=" << snapshot->orders << " continuous_trading=" << snapshot->continuous_trading()
                  << " feed_active=" << snapshot->feed_active << " reg_sho=" << snapshot->reg_sho
                  << " next_sequence=" << book->next_sequence() << '\n';
        return 0;
    }
    std::cout << "accepted_messages=" << messages << " payload_bytes=" << payload_bytes
              << " duplicate_packets=" << duplicates << " next_sequence=" << receiver.next_sequence()
              << " transport_ready=" << receiver.ready() << '\n';
}
