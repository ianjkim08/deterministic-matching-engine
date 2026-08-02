#include "dme/gateway.hpp"

#include <limits>

namespace dme {

bool SessionValidator::violates_risk(const Command& command) const noexcept {
    if (command.type == CommandType::Cancel) return false;
    if (command.quantity == 0 || command.quantity > limits_.maximum_quantity) return true;
    if (command.order_type == OrderType::Market && command.type == CommandType::New) return false;
    if (command.price <= 0 || command.price > limits_.maximum_price) return true;
    const auto price = static_cast<std::uint64_t>(command.price);
    if (price != 0 && command.quantity > limits_.maximum_notional / price) return true;
    return price * command.quantity > limits_.maximum_notional;
}

RejectReason SessionValidator::validate_and_advance(const protocol::Request& request) noexcept {
    if (request.session_id != session_id_ || request.client_sequence != next_sequence_) {
        return RejectReason::InvalidSessionSequence;
    }
    ++next_sequence_;
    if (violates_risk(request.command)) return RejectReason::RiskLimit;
    return RejectReason::None;
}

Event gateway_rejection(const protocol::Request& request, RejectReason reason) noexcept {
    Event event{};
    event.type = EventType::Rejected;
    event.aggressor_side = request.command.side;
    event.reason = reason;
    event.order_id = request.command.order_id;
    event.price = request.command.price;
    event.quantity = request.command.quantity;
    return event;
}

void GatewayEngineRunner::run(const std::atomic<bool>& stop) {
    std::vector<Event> events;
    events.reserve(256);
    GatewayRequest incoming{};
    while (!stop.load(std::memory_order_acquire)) {
        if (!input_.try_pop(incoming)) {
            std::this_thread::yield();
            continue;
        }
        incoming.command.sequence = next_engine_sequence_++;
        if (journal_ != nullptr) journal_->append(incoming.command);
        events.clear();
        book_.process(incoming.command, events);
        for (const Event& event : events) {
            const GatewayResponse response{incoming.session_id, incoming.client_sequence, event};
            while (!output_.try_push(response)) {
                if (stop.load(std::memory_order_acquire)) return;
                std::this_thread::yield();
            }
        }
    }
    if (journal_ != nullptr) journal_->flush();
}

} // namespace dme
