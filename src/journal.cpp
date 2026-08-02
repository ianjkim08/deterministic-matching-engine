#include "dme/journal.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace dme {
namespace {

constexpr std::array<char, 8> journal_magic{'D','M','E','J','N','L','1','\0'};
constexpr std::array<char, 8> snapshot_magic{'D','M','E','S','N','P','1','\0'};

struct FileHeader {
    std::array<char, 8> magic{};
    std::uint32_t version{1};
    std::uint32_t record_size{};
};

struct JournalRecord {
    Command command{};
    std::uint64_t checksum{};
};

struct SnapshotHeader {
    FileHeader file{};
    BookConfig config{};
    Sequence last_sequence{};
    std::uint64_t order_count{};
    std::uint64_t checksum{};
};

std::uint64_t checksum_bytes(const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

template <typename T>
void write_exact(std::ofstream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if (!stream) throw std::runtime_error("persistence write failed");
}

} // namespace

JournalWriter::JournalWriter(const std::filesystem::path& path, bool synchronous)
    : synchronous_(synchronous) {
    const bool new_file = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;
    if (!new_file) {
        std::ifstream existing(path, std::ios::binary);
        FileHeader header{};
        existing.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!existing || header.magic != journal_magic || header.version != 1 ||
            header.record_size != sizeof(JournalRecord)) {
            throw std::runtime_error("refusing to append to an incompatible journal");
        }
    }
    stream_.open(path, std::ios::binary | std::ios::app);
    if (!stream_) throw std::runtime_error("unable to open journal");
    if (new_file) {
        const FileHeader header{journal_magic, 1, static_cast<std::uint32_t>(sizeof(JournalRecord))};
        write_exact(stream_, header);
        stream_.flush();
    }
}

void JournalWriter::append(const Command& command) {
    const JournalRecord record{command, checksum_bytes(&command, sizeof(command))};
    write_exact(stream_, record);
    if (synchronous_) stream_.flush();
}

void JournalWriter::flush() {
    stream_.flush();
    if (!stream_) throw std::runtime_error("journal flush failed");
}

ReplayResult replay_journal(const std::filesystem::path& path,
                            const std::function<void(const Command&)>& consumer) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("unable to open journal for replay");
    FileHeader header{};
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream || header.magic != journal_magic || header.version != 1 ||
        header.record_size != sizeof(JournalRecord)) {
        throw std::runtime_error("invalid journal header");
    }
    ReplayResult result{0, sizeof(header), true};
    JournalRecord record{};
    while (true) {
        stream.read(reinterpret_cast<char*>(&record), sizeof(record));
        if (stream.gcount() == 0) break;
        if (stream.gcount() != static_cast<std::streamsize>(sizeof(record)) ||
            record.checksum != checksum_bytes(&record.command, sizeof(record.command))) {
            result.clean_end = false;
            break;
        }
        consumer(record.command);
        ++result.records;
        result.valid_bytes += sizeof(record);
    }
    return result;
}

void write_snapshot(const std::filesystem::path& path, const OrderBook& book) {
    const auto orders = book.resting_orders();
    SnapshotHeader header{};
    header.file = FileHeader{snapshot_magic, 1, static_cast<std::uint32_t>(sizeof(RestingOrder))};
    header.config = book.config();
    header.last_sequence = book.stats().last_sequence;
    header.order_count = orders.size();
    header.checksum = checksum_bytes(orders.data(), orders.size() * sizeof(RestingOrder));
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("unable to create snapshot");
        write_exact(stream, header);
        if (!orders.empty()) {
            stream.write(reinterpret_cast<const char*>(orders.data()),
                         static_cast<std::streamsize>(orders.size() * sizeof(RestingOrder)));
        }
        stream.flush();
        if (!stream) throw std::runtime_error("snapshot write failed");
    }
#if defined(_WIN32)
    if (MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        const auto error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        std::filesystem::remove(temporary);
        throw std::filesystem::filesystem_error("snapshot replacement failed", temporary, path, error);
    }
#else
    std::filesystem::rename(temporary, path);
#endif
}

OrderBook read_snapshot(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("unable to open snapshot");
    SnapshotHeader header{};
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream || header.file.magic != snapshot_magic || header.file.version != 1 ||
        header.file.record_size != sizeof(RestingOrder) ||
        header.order_count > header.config.maximum_orders) {
        throw std::runtime_error("invalid snapshot header");
    }
    std::vector<RestingOrder> orders(static_cast<std::size_t>(header.order_count));
    if (!orders.empty()) {
        stream.read(reinterpret_cast<char*>(orders.data()),
                    static_cast<std::streamsize>(orders.size() * sizeof(RestingOrder)));
    }
    if (!stream || header.checksum != checksum_bytes(orders.data(), orders.size() * sizeof(RestingOrder))) {
        throw std::runtime_error("corrupt snapshot");
    }
    OrderBook book(header.config);
    book.restore(header.last_sequence, std::move(orders));
    return book;
}

} // namespace dme
