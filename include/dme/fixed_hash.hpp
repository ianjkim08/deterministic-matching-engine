#pragma once

#include "dme/types.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace dme {

// Allocation-free after construction. Linear probing is deterministic and keeps
// the matching path independent of the standard library's node allocator.
class FixedOrderIndex {
public:
    explicit FixedOrderIndex(std::size_t requested_capacity)
        : entries_(table_capacity(requested_capacity)), mask_(entries_.size() - 1U) {}

    [[nodiscard]] bool insert(OrderId id, std::uint32_t node) noexcept {
        std::size_t slot = hash(id) & mask_;
        for (std::size_t probe = 0; probe < entries_.size(); ++probe) {
            Entry& entry = entries_[slot];
            if (entry.state == State::Occupied && entry.id == id) return false;
            if (entry.state == State::Empty) {
                entry = Entry{id, node, State::Occupied};
                return true;
            }
            slot = (slot + 1U) & mask_;
        }
        return false;
    }

    [[nodiscard]] std::uint32_t* find(OrderId id) noexcept {
        std::size_t slot = hash(id) & mask_;
        for (std::size_t probe = 0; probe < entries_.size(); ++probe) {
            Entry& entry = entries_[slot];
            if (entry.state == State::Empty) return nullptr;
            if (entry.state == State::Occupied && entry.id == id) return &entry.node;
            slot = (slot + 1U) & mask_;
        }
        return nullptr;
    }

    [[nodiscard]] const std::uint32_t* find(OrderId id) const noexcept {
        return const_cast<FixedOrderIndex*>(this)->find(id);
    }

    [[nodiscard]] bool erase(OrderId id) noexcept {
        std::size_t slot = hash(id) & mask_;
        for (std::size_t probe = 0; probe < entries_.size(); ++probe) {
            Entry& entry = entries_[slot];
            if (entry.state == State::Empty) return false;
            if (entry.state == State::Occupied && entry.id == id) {
                // Close the probe-chain hole. Reinsert the following cluster so
                // lookups can still stop at the first Empty slot and churn does
                // not accumulate tombstones over a long trading session.
                entry = Entry{};
                std::size_t next = (slot + 1U) & mask_;
                while (entries_[next].state == State::Occupied) {
                    const Entry displaced = entries_[next];
                    entries_[next] = Entry{};
                    static_cast<void>(insert(displaced.id, displaced.node));
                    next = (next + 1U) & mask_;
                }
                return true;
            }
            slot = (slot + 1U) & mask_;
        }
        return false;
    }

private:
    static std::size_t table_capacity(std::size_t requested) {
        constexpr auto digits = std::numeric_limits<std::size_t>::digits;
        constexpr std::size_t largest_safe_request = std::size_t{1} << (digits - 2U);
        if (requested == 0 || requested > largest_safe_request) {
            throw std::invalid_argument("invalid fixed-index capacity");
        }
        return std::bit_ceil(requested * 2U);
    }

    enum class State : std::uint8_t { Empty, Occupied };
    struct Entry {
        OrderId id{};
        std::uint32_t node{};
        State state{State::Empty};
    };

    static constexpr std::size_t hash(OrderId value) noexcept {
        value ^= value >> 30U;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27U;
        value *= 0x94d049bb133111ebULL;
        value ^= value >> 31U;
        return static_cast<std::size_t>(value);
    }

    std::vector<Entry> entries_;
    std::size_t mask_{};
};

} // namespace dme
