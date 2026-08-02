#include "dme/order_book.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace dme {

namespace {
constexpr std::size_t no_price = std::numeric_limits<std::size_t>::max();

std::size_t checked_price_count(const BookConfig& config) {
    if (config.tick_size <= 0 || config.maximum_price < config.minimum_price ||
        config.maximum_orders == 0 ||
        config.maximum_orders > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("invalid order-book configuration");
    }
    if (config.minimum_price < 0 &&
        config.maximum_price > std::numeric_limits<Price>::max() + config.minimum_price) {
        throw std::invalid_argument("price range overflows");
    }
    const Price span = config.maximum_price - config.minimum_price;
    if (span % config.tick_size != 0) {
        throw std::invalid_argument("price range is not tick aligned");
    }
    const auto count = static_cast<std::uint64_t>(span / config.tick_size) + 1U;
    if (count > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("price ladder is too large");
    }
    return static_cast<std::size_t>(count);
}
}

OrderBook::OrderBook(BookConfig config)
    : config_(config),
      price_count_(checked_price_count(config)),
      bids_(price_count_),
      asks_(price_count_),
      bid_bits_((price_count_ + 63U) / 64U),
      ask_bits_((price_count_ + 63U) / 64U),
      nodes_(config.maximum_orders),
      order_index_(config.maximum_orders) {
    free_nodes_.reserve(config.maximum_orders);
    for (std::size_t i = config.maximum_orders; i > 0; --i) {
        free_nodes_.push_back(static_cast<std::uint32_t>(i - 1U));
    }
}

bool OrderBook::valid_price(Price price) const noexcept {
    return price >= config_.minimum_price && price <= config_.maximum_price &&
           (price - config_.minimum_price) % config_.tick_size == 0;
}

std::size_t OrderBook::price_index(Price price) const noexcept {
    return static_cast<std::size_t>((price - config_.minimum_price) / config_.tick_size);
}

Price OrderBook::index_price(std::size_t index) const noexcept {
    return config_.minimum_price + static_cast<Price>(index) * config_.tick_size;
}

void OrderBook::set_active(Side side, std::size_t index) noexcept {
    auto& bits = side == Side::Buy ? bid_bits_ : ask_bits_;
    bits[index / 64U] |= std::uint64_t{1} << (index % 64U);
}

void OrderBook::clear_active(Side side, std::size_t index) noexcept {
    auto& bits = side == Side::Buy ? bid_bits_ : ask_bits_;
    bits[index / 64U] &= ~(std::uint64_t{1} << (index % 64U));
}

std::size_t OrderBook::best_index(Side side) const noexcept {
    const auto& bits = side == Side::Buy ? bid_bits_ : ask_bits_;
    if (side == Side::Sell) {
        for (std::size_t word = 0; word < bits.size(); ++word) {
            if (bits[word] != 0) {
                return word * 64U + static_cast<std::size_t>(std::countr_zero(bits[word]));
            }
        }
    } else {
        for (std::size_t word = bits.size(); word > 0; --word) {
            if (bits[word - 1U] != 0) {
                return (word - 1U) * 64U +
                       (63U - static_cast<std::size_t>(std::countl_zero(bits[word - 1U])));
            }
        }
    }
    return no_price;
}

Price OrderBook::best_bid() const noexcept {
    const auto index = best_index(Side::Buy);
    return index == no_price ? 0 : index_price(index);
}

Price OrderBook::best_ask() const noexcept {
    const auto index = best_index(Side::Sell);
    return index == no_price ? 0 : index_price(index);
}

Quantity OrderBook::quantity_at(Side side, Price price) const noexcept {
    if (!valid_price(price)) return 0;
    const auto index = price_index(price);
    return (side == Side::Buy ? bids_[index] : asks_[index]).total_quantity;
}

bool OrderBook::crosses(Side aggressor, Price limit, std::size_t opposite_index) const noexcept {
    if (opposite_index == no_price) return false;
    const Price opposite = index_price(opposite_index);
    return aggressor == Side::Buy ? opposite <= limit : opposite >= limit;
}

bool OrderBook::can_fully_fill(Side side, Price limit, Quantity quantity) const noexcept {
    Quantity available = 0;
    const auto& levels = side == Side::Buy ? asks_ : bids_;
    const Side opposite = side == Side::Buy ? Side::Sell : Side::Buy;
    auto index = best_index(opposite);
    while (index != no_price && crosses(side, limit, index)) {
        const Quantity at_level = levels[index].total_quantity;
        if (at_level >= quantity - available) return true;
        available += at_level;
        if (side == Side::Buy) {
            if (++index >= price_count_) break;
            while (index < price_count_ && levels[index].count == 0) ++index;
            if (index >= price_count_) break;
        } else {
            if (index == 0) break;
            --index;
            while (levels[index].count == 0) {
                if (index == 0) return false;
                --index;
            }
        }
    }
    return false;
}

void OrderBook::emit(std::vector<Event>& events, EventType type, const Command& command,
                     OrderId contra, Price price, Quantity quantity, RejectReason reason) {
    events.push_back(Event{command.sequence, type, command.side, reason, 0,
                           command.order_id, contra, price, quantity});
}

void OrderBook::add_resting(OrderId id, Side side, Price price, Quantity quantity,
                            Sequence priority, std::vector<Event>& events) {
    const std::uint32_t node_index = free_nodes_.back();
    free_nodes_.pop_back();
    auto& node = nodes_[node_index];
    node = OrderNode{id, price, quantity, priority, invalid_node, invalid_node, side};
    const auto level_index = price_index(price);
    auto& level = side == Side::Buy ? bids_[level_index] : asks_[level_index];
    node.previous = level.tail;
    if (level.tail != invalid_node) nodes_[level.tail].next = node_index;
    else level.head = node_index;
    level.tail = node_index;
    level.total_quantity += quantity;
    ++level.count;
    if (level.count == 1) set_active(side, level_index);
    if (!order_index_.insert(id, node_index)) throw std::logic_error("order index insert failed");
    ++stats_.resting_orders;
    const Command event_command{priority, CommandType::New, side, OrderType::Limit, 0, id, price, quantity};
    emit(events, EventType::Rested, event_command, 0, price, quantity);
}

void OrderBook::remove_node(std::uint32_t node_index) noexcept {
    auto& node = nodes_[node_index];
    const auto level_index = price_index(node.price);
    auto& level = node.side == Side::Buy ? bids_[level_index] : asks_[level_index];
    if (node.previous != invalid_node) nodes_[node.previous].next = node.next;
    else level.head = node.next;
    if (node.next != invalid_node) nodes_[node.next].previous = node.previous;
    else level.tail = node.previous;
    level.total_quantity -= node.remaining;
    --level.count;
    if (level.count == 0) clear_active(node.side, level_index);
    static_cast<void>(order_index_.erase(node.id));
    node = OrderNode{};
    free_nodes_.push_back(node_index);
    --stats_.resting_orders;
}

void OrderBook::submit(const Command& command, std::vector<Event>& events, bool replacement) {
    if (command.quantity == 0) {
        ++stats_.rejected;
        emit(events, EventType::Rejected, command, 0, 0, 0, RejectReason::InvalidQuantity);
        return;
    }
    if (command.order_type != OrderType::Market && !valid_price(command.price)) {
        ++stats_.rejected;
        emit(events, EventType::Rejected, command, 0, 0, 0, RejectReason::InvalidPrice);
        return;
    }
    if (!replacement && order_index_.find(command.order_id) != nullptr) {
        ++stats_.rejected;
        emit(events, EventType::Rejected, command, 0, 0, 0, RejectReason::DuplicateOrderId);
        return;
    }
    const Price effective_limit = command.order_type == OrderType::Market
        ? (command.side == Side::Buy ? config_.maximum_price : config_.minimum_price)
        : command.price;
    if (command.order_type == OrderType::FillOrKill &&
        !can_fully_fill(command.side, effective_limit, command.quantity)) {
        ++stats_.rejected;
        emit(events, EventType::Rejected, command, 0, 0, 0, RejectReason::WouldNotFill);
        return;
    }
    if (free_nodes_.empty() && command.order_type == OrderType::Limit &&
        !can_fully_fill(command.side, effective_limit, command.quantity)) {
        ++stats_.rejected;
        emit(events, EventType::Rejected, command, 0, 0, 0, RejectReason::BookCapacity);
        return;
    }

    if (!replacement) emit(events, EventType::Accepted, command);
    Quantity remaining = command.quantity;
    const Side opposite_side = command.side == Side::Buy ? Side::Sell : Side::Buy;
    while (remaining > 0) {
        const auto opposite_index = best_index(opposite_side);
        if (!crosses(command.side, effective_limit, opposite_index)) break;
        auto& level = opposite_side == Side::Buy ? bids_[opposite_index] : asks_[opposite_index];
        const std::uint32_t resting_index = level.head;
        auto& resting = nodes_[resting_index];
        const Quantity traded = std::min(remaining, resting.remaining);
        remaining -= traded;
        resting.remaining -= traded;
        level.total_quantity -= traded;
        ++stats_.trades;
        emit(events, EventType::Trade, command, resting.id, resting.price, traded);
        if (resting.remaining == 0) remove_node(resting_index);
    }

    if (remaining == 0) return;
    if (command.order_type == OrderType::Limit) {
        add_resting(command.order_id, command.side, command.price, remaining, command.sequence, events);
    } else {
        emit(events, EventType::Cancelled, command, 0, command.price, remaining);
    }
}

void OrderBook::cancel(const Command& command, std::vector<Event>& events, bool replacing) {
    const std::uint32_t* found = order_index_.find(command.order_id);
    if (found == nullptr) {
        ++stats_.rejected;
        emit(events, EventType::Rejected, command, 0, 0, 0, RejectReason::UnknownOrderId);
        return;
    }
    const std::uint32_t index = *found;
    const auto node = nodes_[index];
    remove_node(index);
    if (!replacing) emit(events, EventType::Cancelled, command, 0, node.price, node.remaining);
}

void OrderBook::replace(const Command& command, std::vector<Event>& events) {
    const std::uint32_t* found = order_index_.find(command.order_id);
    if (found == nullptr) {
        ++stats_.rejected;
        emit(events, EventType::Rejected, command, 0, 0, 0, RejectReason::UnknownOrderId);
        return;
    }
    if (command.quantity == 0 || !valid_price(command.price)) {
        ++stats_.rejected;
        emit(events, EventType::Rejected, command, 0, 0, 0,
             command.quantity == 0 ? RejectReason::InvalidQuantity : RejectReason::InvalidPrice);
        return;
    }
    const Side original_side = nodes_[*found].side;
    cancel(command, events, true);
    Command replacement = command;
    replacement.side = original_side;
    replacement.order_type = OrderType::Limit;
    emit(events, EventType::Replaced, replacement, 0, replacement.price, replacement.quantity);
    submit(replacement, events, true);
}

void OrderBook::process(const Command& command, std::vector<Event>& events) {
    if (command.sequence != stats_.last_sequence + 1U) {
        ++stats_.rejected;
        emit(events, EventType::Rejected, command, 0, 0, 0, RejectReason::SequenceGap);
        return;
    }
    stats_.last_sequence = command.sequence;
    ++stats_.commands;
    switch (command.type) {
        case CommandType::New: submit(command, events); break;
        case CommandType::Cancel: cancel(command, events); break;
        case CommandType::Replace: replace(command, events); break;
    }
}

std::vector<RestingOrder> OrderBook::resting_orders() const {
    std::vector<RestingOrder> result;
    result.reserve(stats_.resting_orders);
    for (std::size_t price = 0; price < price_count_; ++price) {
        for (Side side : {Side::Buy, Side::Sell}) {
            const auto& level = side == Side::Buy ? bids_[price] : asks_[price];
            auto node = level.head;
            while (node != invalid_node) {
                const auto& order = nodes_[node];
                result.push_back({order.id, order.side, order.price, order.remaining, order.priority});
                node = order.next;
            }
        }
    }
    return result;
}

void OrderBook::restore(Sequence last_sequence, std::vector<RestingOrder> orders) {
    if (stats_.resting_orders != 0 || stats_.last_sequence != 0 ||
        orders.size() > config_.maximum_orders) {
        throw std::logic_error("restore requires an empty book with sufficient capacity");
    }
    std::stable_sort(orders.begin(), orders.end(), [](const auto& left, const auto& right) {
        return left.priority < right.priority;
    });
    std::vector<Event> ignored;
    ignored.reserve(orders.size());
    for (const auto& order : orders) {
        if (!valid_price(order.price) || order.quantity == 0 || order.priority > last_sequence ||
            order_index_.find(order.order_id) != nullptr) {
            throw std::runtime_error("invalid resting order in snapshot");
        }
        add_resting(order.order_id, order.side, order.price, order.quantity, order.priority, ignored);
    }
    stats_.last_sequence = last_sequence;
}

} // namespace dme
