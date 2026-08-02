#include "dme/order_book.hpp"
#include "dme/protocol.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
#define CHECK(expression) do { if (!(expression)) throw Failure(#expression); } while (false)

class ReferenceBook {
public:
    explicit ReferenceBook(dme::BookConfig config) : config_(config) {}

    void process(const dme::Command& command, std::vector<dme::Event>& events) {
        if (command.sequence != last_sequence_ + 1U) {
            emit(events, dme::EventType::Rejected, command, 0, 0, 0,
                 dme::RejectReason::SequenceGap);
            return;
        }
        last_sequence_ = command.sequence;
        switch (command.type) {
            case dme::CommandType::New: submit(command, events, false); break;
            case dme::CommandType::Cancel: cancel(command, events, false); break;
            case dme::CommandType::Replace: replace(command, events); break;
        }
    }

    std::vector<dme::RestingOrder> orders() const {
        std::vector<dme::RestingOrder> result;
        result.reserve(orders_.size());
        for (const auto& order : orders_) {
            result.push_back({order.id, order.side, order.price, order.quantity, order.priority});
        }
        return result;
    }

private:
    struct Order {
        dme::OrderId id{};
        dme::Side side{dme::Side::Buy};
        dme::Price price{};
        dme::Quantity quantity{};
        dme::Sequence priority{};
    };

    bool valid_price(dme::Price price) const {
        return price >= config_.minimum_price && price <= config_.maximum_price &&
               (price - config_.minimum_price) % config_.tick_size == 0;
    }

    auto find(dme::OrderId id) {
        return std::find_if(orders_.begin(), orders_.end(),
                            [id](const Order& order) { return order.id == id; });
    }

    bool crosses(dme::Side side, dme::Price limit, const Order& resting) const {
        return side == dme::Side::Buy ? resting.price <= limit : resting.price >= limit;
    }

    auto best_opposite(dme::Side side, dme::Price limit) {
        auto best = orders_.end();
        for (auto current = orders_.begin(); current != orders_.end(); ++current) {
            if (current->side == side || !crosses(side, limit, *current)) continue;
            if (best == orders_.end()) {
                best = current;
                continue;
            }
            const bool better_price = side == dme::Side::Buy
                ? current->price < best->price : current->price > best->price;
            if (better_price || (current->price == best->price &&
                                 current->priority < best->priority)) best = current;
        }
        return best;
    }

    bool can_fill(dme::Side side, dme::Price limit, dme::Quantity quantity) const {
        dme::Quantity available = 0;
        for (const auto& order : orders_) {
            if (order.side != side && crosses(side, limit, order)) {
                if (order.quantity >= quantity - available) return true;
                available += order.quantity;
            }
        }
        return false;
    }

    void emit(std::vector<dme::Event>& events, dme::EventType type,
              const dme::Command& command, dme::OrderId contra = 0,
              dme::Price price = 0, dme::Quantity quantity = 0,
              dme::RejectReason reason = dme::RejectReason::None) {
        events.push_back({command.sequence, type, command.side, reason, 0,
                          command.order_id, contra, price, quantity});
    }

    void submit(const dme::Command& command, std::vector<dme::Event>& events, bool replacement) {
        if (command.quantity == 0) {
            emit(events, dme::EventType::Rejected, command, 0, 0, 0,
                 dme::RejectReason::InvalidQuantity);
            return;
        }
        if (command.order_type != dme::OrderType::Market && !valid_price(command.price)) {
            emit(events, dme::EventType::Rejected, command, 0, 0, 0,
                 dme::RejectReason::InvalidPrice);
            return;
        }
        if (!replacement && find(command.order_id) != orders_.end()) {
            emit(events, dme::EventType::Rejected, command, 0, 0, 0,
                 dme::RejectReason::DuplicateOrderId);
            return;
        }
        const dme::Price limit = command.order_type == dme::OrderType::Market
            ? (command.side == dme::Side::Buy ? config_.maximum_price : config_.minimum_price)
            : command.price;
        if (command.order_type == dme::OrderType::FillOrKill &&
            !can_fill(command.side, limit, command.quantity)) {
            emit(events, dme::EventType::Rejected, command, 0, 0, 0,
                 dme::RejectReason::WouldNotFill);
            return;
        }
        if (!replacement) emit(events, dme::EventType::Accepted, command);
        dme::Quantity remaining = command.quantity;
        while (remaining != 0) {
            auto resting = best_opposite(command.side, limit);
            if (resting == orders_.end()) break;
            const dme::Quantity traded = std::min(remaining, resting->quantity);
            const auto contra = resting->id;
            const auto price = resting->price;
            remaining -= traded;
            resting->quantity -= traded;
            emit(events, dme::EventType::Trade, command, contra, price, traded);
            if (resting->quantity == 0) orders_.erase(resting);
        }
        if (remaining == 0) return;
        if (command.order_type == dme::OrderType::Limit) {
            orders_.push_back({command.order_id, command.side, command.price,
                               remaining, command.sequence});
            emit(events, dme::EventType::Rested, command, 0, command.price, remaining);
        } else {
            emit(events, dme::EventType::Cancelled, command, 0, command.price, remaining);
        }
    }

    void cancel(const dme::Command& command, std::vector<dme::Event>& events, bool replacing) {
        const auto order = find(command.order_id);
        if (order == orders_.end()) {
            emit(events, dme::EventType::Rejected, command, 0, 0, 0,
                 dme::RejectReason::UnknownOrderId);
            return;
        }
        const auto removed = *order;
        orders_.erase(order);
        if (!replacing) emit(events, dme::EventType::Cancelled, command, 0,
                             removed.price, removed.quantity);
    }

    void replace(const dme::Command& command, std::vector<dme::Event>& events) {
        const auto order = find(command.order_id);
        if (order == orders_.end()) {
            emit(events, dme::EventType::Rejected, command, 0, 0, 0,
                 dme::RejectReason::UnknownOrderId);
            return;
        }
        if (command.quantity == 0 || !valid_price(command.price)) {
            emit(events, dme::EventType::Rejected, command, 0, 0, 0,
                 command.quantity == 0 ? dme::RejectReason::InvalidQuantity
                                       : dme::RejectReason::InvalidPrice);
            return;
        }
        const dme::Side side = order->side;
        cancel(command, events, true);
        dme::Command replacement = command;
        replacement.side = side;
        replacement.order_type = dme::OrderType::Limit;
        emit(events, dme::EventType::Replaced, replacement, 0,
             replacement.price, replacement.quantity);
        submit(replacement, events, true);
    }

    dme::BookConfig config_;
    dme::Sequence last_sequence_{};
    std::vector<Order> orders_;
};

std::uint64_t next_random(std::uint64_t& state) {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

bool equal_event(const dme::Event& left, const dme::Event& right) {
    return left.sequence == right.sequence && left.type == right.type &&
           left.aggressor_side == right.aggressor_side && left.reason == right.reason &&
           left.order_id == right.order_id && left.contra_order_id == right.contra_order_id &&
           left.price == right.price && left.quantity == right.quantity;
}

void randomized_differential() {
    const dme::BookConfig config{90, 110, 1, 10'000};
    dme::OrderBook optimized(config);
    ReferenceBook reference(config);
    std::vector<dme::Event> actual;
    std::vector<dme::Event> expected;
    actual.reserve(64);
    expected.reserve(64);
    std::uint64_t random = 0x8f3d'91a2'74c6'5be1ULL;
    constexpr std::uint64_t iterations = 50'000;
    for (std::uint64_t sequence = 1; sequence <= iterations; ++sequence) {
        const auto value = next_random(random);
        dme::Command command{};
        command.sequence = sequence;
        command.order_id = 1U + value % 2'000U;
        command.side = ((value >> 12U) & 1U) == 0 ? dme::Side::Buy : dme::Side::Sell;
        command.price = 95 + static_cast<dme::Price>((value >> 16U) % 11U);
        command.quantity = 1U + ((value >> 24U) % 20U);
        const auto operation = value % 100U;
        if (operation < 55U) {
            command.type = dme::CommandType::New;
            const auto kind = (value >> 32U) % 100U;
            if (kind < 65U) command.order_type = dme::OrderType::Limit;
            else if (kind < 85U) command.order_type = dme::OrderType::ImmediateOrCancel;
            else if (kind < 95U) command.order_type = dme::OrderType::FillOrKill;
            else {
                command.order_type = dme::OrderType::Market;
                command.price = 0;
            }
        } else if (operation < 80U) {
            command.type = dme::CommandType::Cancel;
        } else {
            command.type = dme::CommandType::Replace;
            command.order_type = dme::OrderType::Limit;
        }
        actual.clear();
        expected.clear();
        optimized.process(command, actual);
        reference.process(command, expected);
        CHECK(actual.size() == expected.size());
        for (std::size_t index = 0; index < actual.size(); ++index) {
            CHECK(equal_event(actual[index], expected[index]));
        }
        if (sequence % 100U == 0) {
            auto actual_orders = optimized.resting_orders();
            auto expected_orders = reference.orders();
            const auto by_id = [](const auto& left, const auto& right) {
                return left.order_id < right.order_id;
            };
            std::sort(actual_orders.begin(), actual_orders.end(), by_id);
            std::sort(expected_orders.begin(), expected_orders.end(), by_id);
            CHECK(actual_orders.size() == expected_orders.size());
            for (std::size_t index = 0; index < actual_orders.size(); ++index) {
                CHECK(actual_orders[index].order_id == expected_orders[index].order_id);
                CHECK(actual_orders[index].side == expected_orders[index].side);
                CHECK(actual_orders[index].price == expected_orders[index].price);
                CHECK(actual_orders[index].quantity == expected_orders[index].quantity);
                CHECK(actual_orders[index].priority == expected_orders[index].priority);
            }
        }
    }
}

void malformed_protocol_fuzz() {
    std::array<std::byte, dme::protocol::maximum_frame_size> bytes{};
    std::uint64_t random = 0xd134'2543'de82'ef95ULL;
    for (std::size_t iteration = 0; iteration < 100'000; ++iteration) {
        const std::size_t length = next_random(random) % (bytes.size() + 1U);
        for (std::size_t index = 0; index < length; ++index) {
            bytes[index] = static_cast<std::byte>(next_random(random) & 0xffU);
        }
        const auto request = dme::protocol::decode_request({bytes.data(), length});
        const auto response = dme::protocol::decode_response({bytes.data(), length});
        CHECK(request.bytes_consumed <= length);
        CHECK(response.bytes_consumed <= length);
    }
}

} // namespace

int main() {
    try {
        randomized_differential();
        std::cout << "PASS randomized_differential (50000 commands)\n";
        malformed_protocol_fuzz();
        std::cout << "PASS malformed_protocol_fuzz (100000 frames)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL differential suite: " << error.what() << '\n';
        return 1;
    }
}
