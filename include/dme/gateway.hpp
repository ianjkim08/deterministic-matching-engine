#pragma once

#include "dme/journal.hpp"
#include "dme/order_book.hpp"
#include "dme/protocol.hpp"
#include "dme/spsc_queue.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace dme {

struct RiskLimits {
    Quantity maximum_quantity{1'000'000};
    Price maximum_price{1'000'000'000};
    std::uint64_t maximum_notional{100'000'000'000ULL};
};

struct GatewayRequest {
    std::uint32_t session_id{};
    std::uint64_t client_sequence{};
    Command command{};
};

struct GatewayResponse {
    std::uint32_t session_id{};
    std::uint64_t client_sequence{};
    Event event{};
};

static_assert(std::is_trivially_copyable_v<GatewayRequest>);
static_assert(std::is_trivially_copyable_v<GatewayResponse>);

class SessionValidator {
public:
    SessionValidator(std::uint32_t session_id, RiskLimits limits = {})
        : session_id_(session_id), limits_(limits) {}

    [[nodiscard]] RejectReason validate_and_advance(const protocol::Request& request) noexcept;
    [[nodiscard]] std::uint64_t next_client_sequence() const noexcept { return next_sequence_; }

private:
    [[nodiscard]] bool violates_risk(const Command& command) const noexcept;

    std::uint32_t session_id_{};
    std::uint64_t next_sequence_{1};
    RiskLimits limits_{};
};

// Attaches session correlation to every engine event while retaining a single
// writer for book state and engine sequence assignment.
class GatewayEngineRunner {
public:
    GatewayEngineRunner(OrderBook& book, SpscQueue<GatewayRequest>& input,
                        SpscQueue<GatewayResponse>& output, JournalWriter* journal = nullptr)
        : book_(book), input_(input), output_(output), journal_(journal),
          next_engine_sequence_(book.stats().last_sequence + 1U) {}

    void run(const std::atomic<bool>& stop);

private:
    OrderBook& book_;
    SpscQueue<GatewayRequest>& input_;
    SpscQueue<GatewayResponse>& output_;
    JournalWriter* journal_{};
    Sequence next_engine_sequence_{};
};

[[nodiscard]] Event gateway_rejection(const protocol::Request& request,
                                      RejectReason reason) noexcept;

} // namespace dme
