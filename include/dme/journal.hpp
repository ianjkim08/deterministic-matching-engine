#pragma once

#include "dme/order_book.hpp"

#include <filesystem>
#include <fstream>
#include <functional>

namespace dme {

class JournalWriter {
public:
    explicit JournalWriter(const std::filesystem::path& path, bool synchronous = false);
    void append(const Command& command);
    void flush();

private:
    std::ofstream stream_;
    bool synchronous_{};
};

struct ReplayResult {
    std::uint64_t records{};
    std::uint64_t valid_bytes{};
    bool clean_end{true};
};

// Stops at the first torn or corrupt record. A caller can safely truncate to
// valid_bytes before accepting new traffic.
[[nodiscard]] ReplayResult replay_journal(
    const std::filesystem::path& path,
    const std::function<void(const Command&)>& consumer);

void write_snapshot(const std::filesystem::path& path, const OrderBook& book);
[[nodiscard]] OrderBook read_snapshot(const std::filesystem::path& path);

} // namespace dme
