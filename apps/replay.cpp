#include "dme/journal.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: dme_replay JOURNAL\n";
        return 2;
    }
    try {
        dme::OrderBook book({1, 1'000'000, 1, 1'000'000});
        std::vector<dme::Event> events;
        events.reserve(32);
        const auto result = dme::replay_journal(argv[1], [&](const dme::Command& command) {
            events.clear();
            book.process(command, events);
        });
        std::cout << "records=" << result.records
                  << " valid_bytes=" << result.valid_bytes
                  << " clean_end=" << (result.clean_end ? "yes" : "no")
                  << " resting_orders=" << book.stats().resting_orders
                  << " trades=" << book.stats().trades
                  << " rejected=" << book.stats().rejected << '\n';
        return result.clean_end && book.stats().rejected == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "replay failed: " << error.what() << '\n';
        return 1;
    }
}
