#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace dme {

using OrderId = std::uint64_t;
using Sequence = std::uint64_t;
using Price = std::int64_t;      // integer ticks/currency subunits; never floating point
using Quantity = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market, ImmediateOrCancel, FillOrKill };
enum class CommandType : std::uint8_t { New, Cancel, Replace };

enum class EventType : std::uint8_t {
    Accepted,
    Rejected,
    Trade,
    Cancelled,
    Replaced,
    Rested
};

enum class RejectReason : std::uint8_t {
    None,
    InvalidQuantity,
    InvalidPrice,
    DuplicateOrderId,
    UnknownOrderId,
    BookCapacity,
    WouldNotFill,
    SequenceGap
};

struct Command {
    Sequence sequence{};
    CommandType type{CommandType::New};
    Side side{Side::Buy};
    OrderType order_type{OrderType::Limit};
    std::uint8_t reserved{};
    OrderId order_id{};
    Price price{};
    Quantity quantity{};
};

struct Event {
    Sequence sequence{};
    EventType type{EventType::Accepted};
    Side aggressor_side{Side::Buy};
    RejectReason reason{RejectReason::None};
    std::uint8_t reserved{};
    OrderId order_id{};
    OrderId contra_order_id{};
    Price price{};
    Quantity quantity{};
};

static_assert(std::is_trivially_copyable_v<Command>);
static_assert(std::is_trivially_copyable_v<Event>);

constexpr std::string_view to_string(EventType value) noexcept {
    switch (value) {
        case EventType::Accepted: return "accepted";
        case EventType::Rejected: return "rejected";
        case EventType::Trade: return "trade";
        case EventType::Cancelled: return "cancelled";
        case EventType::Replaced: return "replaced";
        case EventType::Rested: return "rested";
    }
    return "unknown";
}

} // namespace dme
