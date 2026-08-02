#pragma once

#include "dme/fixed_hash.hpp"
#include "dme/types.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace dme {

struct BookConfig {
    Price minimum_price{1};
    Price maximum_price{1'000'000};
    Price tick_size{1};
    std::size_t maximum_orders{1'000'000};
};

struct BookStats {
    Sequence last_sequence{};
    std::uint64_t commands{};
    std::uint64_t trades{};
    std::uint64_t rejected{};
    std::size_t resting_orders{};
};

struct RestingOrder {
    OrderId order_id{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{};
    Sequence priority{};
};

class OrderBook {
public:
    explicit OrderBook(BookConfig config);

    // Events are appended. Reserve this vector before a benchmark to keep the
    // matching path allocation-free.
    void process(const Command& command, std::vector<Event>& events);

    [[nodiscard]] Price best_bid() const noexcept;
    [[nodiscard]] Price best_ask() const noexcept;
    [[nodiscard]] Quantity quantity_at(Side side, Price price) const noexcept;
    [[nodiscard]] const BookStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const BookConfig& config() const noexcept { return config_; }
    [[nodiscard]] std::vector<RestingOrder> resting_orders() const;
    // Recovery hook: valid only on a newly constructed, empty book.
    void restore(Sequence last_sequence, std::vector<RestingOrder> orders);

private:
    static constexpr std::uint32_t invalid_node = std::numeric_limits<std::uint32_t>::max();

    struct OrderNode {
        OrderId id{};
        Price price{};
        Quantity remaining{};
        Sequence priority{};
        std::uint32_t previous{invalid_node};
        std::uint32_t next{invalid_node};
        Side side{Side::Buy};
    };

    struct PriceLevel {
        Quantity total_quantity{};
        std::uint32_t head{invalid_node};
        std::uint32_t tail{invalid_node};
        std::uint32_t count{};
    };

    [[nodiscard]] bool valid_price(Price price) const noexcept;
    [[nodiscard]] std::size_t price_index(Price price) const noexcept;
    [[nodiscard]] Price index_price(std::size_t index) const noexcept;
    [[nodiscard]] std::size_t best_index(Side side) const noexcept;
    [[nodiscard]] bool crosses(Side aggressor, Price limit, std::size_t opposite_index) const noexcept;
    [[nodiscard]] bool can_fully_fill(Side side, Price limit, Quantity quantity) const noexcept;

    void set_active(Side side, std::size_t index) noexcept;
    void clear_active(Side side, std::size_t index) noexcept;
    void add_resting(OrderId id, Side side, Price price, Quantity quantity, Sequence priority,
                     std::vector<Event>& events);
    void remove_node(std::uint32_t node_index) noexcept;
    void cancel(const Command& command, std::vector<Event>& events, bool replacing = false);
    void replace(const Command& command, std::vector<Event>& events);
    void submit(const Command& command, std::vector<Event>& events, bool replacement = false);
    void emit(std::vector<Event>& events, EventType type, const Command& command,
              OrderId contra = 0, Price price = 0, Quantity quantity = 0,
              RejectReason reason = RejectReason::None);

    BookConfig config_;
    std::size_t price_count_{};
    std::vector<PriceLevel> bids_;
    std::vector<PriceLevel> asks_;
    std::vector<std::uint64_t> bid_bits_;
    std::vector<std::uint64_t> ask_bits_;
    std::vector<OrderNode> nodes_;
    std::vector<std::uint32_t> free_nodes_;
    FixedOrderIndex order_index_;
    BookStats stats_;
};

} // namespace dme
