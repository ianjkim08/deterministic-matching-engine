#include "dme/engine_runner.hpp"
#include "dme/gateway.hpp"
#include "dme/journal.hpp"
#include "dme/protocol.hpp"
#include "dme/spsc_queue.hpp"

#include <atomic>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct TestFailure : std::runtime_error { using std::runtime_error::runtime_error; };

#define CHECK(expression) do { if (!(expression)) throw TestFailure(#expression); } while (false)

dme::Command order(dme::Sequence sequence, dme::OrderId id, dme::Side side,
                   dme::Price price, dme::Quantity quantity,
                   dme::OrderType type = dme::OrderType::Limit) {
    return {sequence, dme::CommandType::New, side, type, 0, id, price, quantity};
}

void price_time_priority() {
    dme::OrderBook book({90, 110, 1, 32});
    std::vector<dme::Event> events;
    book.process(order(1, 1, dme::Side::Sell, 100, 5), events);
    book.process(order(2, 2, dme::Side::Sell, 100, 7), events);
    events.clear();
    book.process(order(3, 3, dme::Side::Buy, 101, 8), events);
    CHECK(events.size() == 3);
    CHECK(events[1].type == dme::EventType::Trade && events[1].contra_order_id == 1 && events[1].quantity == 5);
    CHECK(events[2].type == dme::EventType::Trade && events[2].contra_order_id == 2 && events[2].quantity == 3);
    CHECK(book.quantity_at(dme::Side::Sell, 100) == 4);
    CHECK(book.best_ask() == 100);
}

void best_price_and_partial_fill() {
    dme::OrderBook book({90, 110, 1, 32});
    std::vector<dme::Event> events;
    book.process(order(1, 1, dme::Side::Sell, 102, 10), events);
    book.process(order(2, 2, dme::Side::Sell, 99, 4), events);
    events.clear();
    book.process(order(3, 3, dme::Side::Buy, 102, 6), events);
    CHECK(events[1].contra_order_id == 2 && events[1].price == 99 && events[1].quantity == 4);
    CHECK(events[2].contra_order_id == 1 && events[2].price == 102 && events[2].quantity == 2);
    CHECK(book.quantity_at(dme::Side::Sell, 102) == 8);
}

void cancel_and_replace_loses_priority() {
    dme::OrderBook book({90, 110, 1, 32});
    std::vector<dme::Event> events;
    book.process(order(1, 10, dme::Side::Buy, 100, 5), events);
    book.process(order(2, 11, dme::Side::Buy, 100, 5), events);
    dme::Command replace{3, dme::CommandType::Replace, dme::Side::Buy,
                         dme::OrderType::Limit, 0, 10, 100, 5};
    book.process(replace, events);
    events.clear();
    book.process(order(4, 12, dme::Side::Sell, 100, 5), events);
    CHECK(events[1].contra_order_id == 11);
    dme::Command cancel{5, dme::CommandType::Cancel, dme::Side::Buy,
                        dme::OrderType::Limit, 0, 10, 0, 0};
    book.process(cancel, events);
    CHECK(book.stats().resting_orders == 0);
}

void immediate_and_fill_or_kill() {
    dme::OrderBook book({90, 110, 1, 32});
    std::vector<dme::Event> events;
    book.process(order(1, 1, dme::Side::Sell, 100, 5), events);
    events.clear();
    book.process(order(2, 2, dme::Side::Buy, 100, 7, dme::OrderType::FillOrKill), events);
    CHECK(events.size() == 1 && events[0].reason == dme::RejectReason::WouldNotFill);
    CHECK(book.quantity_at(dme::Side::Sell, 100) == 5);
    events.clear();
    book.process(order(3, 3, dme::Side::Buy, 100, 7, dme::OrderType::ImmediateOrCancel), events);
    CHECK(events.size() == 3 && events.back().type == dme::EventType::Cancelled);
    CHECK(events.back().quantity == 2 && book.stats().resting_orders == 0);
}

void rejects_gaps_and_duplicates() {
    dme::OrderBook book({90, 110, 1, 4});
    std::vector<dme::Event> events;
    book.process(order(2, 1, dme::Side::Buy, 100, 1), events);
    CHECK(events.back().reason == dme::RejectReason::SequenceGap);
    book.process(order(1, 1, dme::Side::Buy, 100, 1), events);
    book.process(order(2, 1, dme::Side::Buy, 100, 1), events);
    CHECK(events.back().reason == dme::RejectReason::DuplicateOrderId);
}

void executable_order_is_allowed_when_book_is_full() {
    dme::OrderBook book({90, 110, 1, 1});
    std::vector<dme::Event> events;
    book.process(order(1, 1, dme::Side::Sell, 100, 5), events);
    CHECK(book.stats().resting_orders == 1);
    events.clear();
    book.process(order(2, 2, dme::Side::Buy, 100, 5,
                       dme::OrderType::ImmediateOrCancel), events);
    CHECK(events.size() == 2);
    CHECK(events[0].type == dme::EventType::Accepted);
    CHECK(events[1].type == dme::EventType::Trade);
    CHECK(book.stats().resting_orders == 0);
}

void snapshot_round_trip() {
    dme::OrderBook book({90, 110, 1, 32});
    std::vector<dme::Event> events;
    book.process(order(1, 1, dme::Side::Buy, 99, 3), events);
    book.process(order(2, 2, dme::Side::Sell, 102, 7), events);
    const auto path = std::filesystem::temp_directory_path() / "dme_test.snapshot";
    std::filesystem::remove(path);
    dme::write_snapshot(path, book);
    auto restored = dme::read_snapshot(path);
    CHECK(restored.stats().last_sequence == 2);
    CHECK(restored.quantity_at(dme::Side::Buy, 99) == 3);
    CHECK(restored.quantity_at(dme::Side::Sell, 102) == 7);
    book.process(order(3, 3, dme::Side::Buy, 98, 2), events);
    dme::write_snapshot(path, book);
    auto replaced = dme::read_snapshot(path);
    CHECK(replaced.stats().last_sequence == 3);
    CHECK(replaced.quantity_at(dme::Side::Buy, 98) == 2);
    std::filesystem::remove(path);
}

void journal_round_trip() {
    const auto path = std::filesystem::temp_directory_path() / "dme_test.journal";
    std::filesystem::remove(path);
    {
        dme::JournalWriter writer(path);
        writer.append(order(1, 1, dme::Side::Buy, 99, 3));
        writer.append(order(2, 2, dme::Side::Sell, 99, 2));
        writer.flush();
    }
    dme::OrderBook replayed({90, 110, 1, 32});
    std::vector<dme::Event> events;
    const auto result = dme::replay_journal(path, [&](const auto& command) {
        events.clear();
        replayed.process(command, events);
    });
    CHECK(result.clean_end && result.records == 2);
    CHECK(replayed.stats().trades == 1);
    CHECK(replayed.quantity_at(dme::Side::Buy, 99) == 1);
    std::filesystem::remove(path);
}

void journal_rejects_bad_header_and_detects_torn_tail() {
    const auto bad_path = std::filesystem::temp_directory_path() / "dme_test_bad.journal";
    std::filesystem::remove(bad_path);
    {
        std::ofstream bad(bad_path, std::ios::binary);
        bad << "not a journal";
    }
    bool rejected = false;
    try {
        dme::JournalWriter writer(bad_path);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    CHECK(rejected);
    std::filesystem::remove(bad_path);

    const auto torn_path = std::filesystem::temp_directory_path() / "dme_test_torn.journal";
    std::filesystem::remove(torn_path);
    {
        dme::JournalWriter writer(torn_path);
        writer.append(order(1, 1, dme::Side::Buy, 99, 3));
        writer.append(order(2, 2, dme::Side::Sell, 99, 2));
        writer.flush();
    }
    std::filesystem::resize_file(torn_path, std::filesystem::file_size(torn_path) - 1U);
    std::uint64_t replayed = 0;
    const auto result = dme::replay_journal(torn_path, [&](const auto&) { ++replayed; });
    CHECK(!result.clean_end && result.records == 1 && replayed == 1);
    std::filesystem::remove(torn_path);
}

void queue_orders_cross_thread() {
    constexpr std::uint64_t count = 200'000;
    dme::SpscQueue<std::uint64_t> queue(1024);
    std::atomic<bool> start{false};
    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (std::uint64_t i = 1; i <= count; ++i) while (!queue.try_push(i)) {}
    });
    start.store(true, std::memory_order_release);
    for (std::uint64_t expected = 1; expected <= count; ++expected) {
        std::uint64_t actual{};
        while (!queue.try_pop(actual)) {}
        CHECK(actual == expected);
    }
    producer.join();
}

void engine_runner_preserves_output() {
    dme::OrderBook book({90, 110, 1, 32});
    dme::SpscQueue<dme::Command> input(16);
    dme::SpscQueue<dme::Event> output(32);
    std::atomic<bool> stop{false};
    dme::EngineRunner runner(book, input, output);
    std::thread core([&] { runner.run(stop); });
    const auto sell = order(1, 1, dme::Side::Sell, 100, 5);
    const auto buy = order(2, 2, dme::Side::Buy, 100, 5);
    while (!input.try_push(sell)) {}
    while (!input.try_push(buy)) {}
    std::vector<dme::Event> received;
    while (received.size() < 4) {
        dme::Event event{};
        if (output.try_pop(event)) received.push_back(event);
    }
    stop.store(true, std::memory_order_release);
    core.join();
    CHECK(received[0].type == dme::EventType::Accepted);
    CHECK(received[1].type == dme::EventType::Rested);
    CHECK(received[2].type == dme::EventType::Accepted);
    CHECK(received[3].type == dme::EventType::Trade);
}

void protocol_round_trips_and_handles_fragmentation() {
    const std::vector<dme::Command> commands{
        order(0, 11, dme::Side::Buy, 101, 7),
        {0, dme::CommandType::Cancel, dme::Side::Buy, dme::OrderType::Limit, 0, 11, 0, 0},
        {0, dme::CommandType::Replace, dme::Side::Sell, dme::OrderType::Limit, 0, 11, 102, 9}
    };
    std::array<std::byte, dme::protocol::maximum_frame_size> buffer{};
    std::uint64_t sequence = 1;
    for (const auto& command : commands) {
        const dme::protocol::Request request{42, sequence++, command};
        std::size_t written{};
        CHECK(dme::protocol::encode_request(request, buffer, written));
        for (std::size_t size = 0; size < written; ++size) {
            CHECK(dme::protocol::decode_request({buffer.data(), size}).status ==
                  dme::protocol::DecodeStatus::NeedMoreData);
        }
        const auto decoded = dme::protocol::decode_request({buffer.data(), written});
        CHECK(decoded.status == dme::protocol::DecodeStatus::Ok);
        CHECK(decoded.bytes_consumed == written);
        CHECK(decoded.request.session_id == request.session_id);
        CHECK(decoded.request.client_sequence == request.client_sequence);
        CHECK(decoded.request.command.type == command.type);
        CHECK(decoded.request.command.order_id == command.order_id);
        CHECK(decoded.request.command.price == command.price);
        CHECK(decoded.request.command.quantity == command.quantity);
    }

    dme::Event event{99, dme::EventType::Trade, dme::Side::Buy, dme::RejectReason::None,
                     0, 11, 12, 101, 5};
    std::size_t written{};
    CHECK(dme::protocol::encode_response({42, 4, event}, buffer, written));
    const auto response = dme::protocol::decode_response({buffer.data(), written});
    CHECK(response.status == dme::protocol::DecodeStatus::Ok);
    CHECK(response.response.session_id == 42 && response.response.client_sequence == 4);
    CHECK(response.response.event.sequence == 99 && response.response.event.contra_order_id == 12);
}

void protocol_rejects_malformed_frames() {
    std::array<std::byte, dme::protocol::maximum_frame_size> buffer{};
    std::size_t written{};
    CHECK(dme::protocol::encode_request({7, 1, order(0, 1, dme::Side::Buy, 100, 2)},
                                        buffer, written));
    auto corrupted = buffer;
    corrupted[0] ^= std::byte{0xff};
    CHECK(dme::protocol::decode_request({corrupted.data(), written}).status ==
          dme::protocol::DecodeStatus::InvalidMagic);
    corrupted = buffer;
    corrupted[4] = std::byte{99};
    CHECK(dme::protocol::decode_request({corrupted.data(), written}).status ==
          dme::protocol::DecodeStatus::UnsupportedVersion);
    corrupted = buffer;
    corrupted[5] = std::byte{99};
    CHECK(dme::protocol::decode_request({corrupted.data(), written}).status ==
          dme::protocol::DecodeStatus::UnknownMessageType);
    corrupted = buffer;
    corrupted[dme::protocol::header_size] = std::byte{9};
    CHECK(dme::protocol::decode_request({corrupted.data(), written}).status ==
          dme::protocol::DecodeStatus::InvalidField);
}

void session_sequence_and_risk_validation() {
    dme::SessionValidator validator(42, {100, 1'000, 50'000});
    auto request = dme::protocol::Request{42, 1, order(0, 1, dme::Side::Buy, 100, 10)};
    CHECK(validator.validate_and_advance(request) == dme::RejectReason::None);
    CHECK(validator.next_client_sequence() == 2);
    CHECK(validator.validate_and_advance(request) == dme::RejectReason::InvalidSessionSequence);
    request.client_sequence = 2;
    request.command.quantity = 101;
    CHECK(validator.validate_and_advance(request) == dme::RejectReason::RiskLimit);
    request.client_sequence = 3;
    request.command.quantity = std::numeric_limits<dme::Quantity>::max();
    request.command.price = std::numeric_limits<dme::Price>::max();
    CHECK(validator.validate_and_advance(request) == dme::RejectReason::RiskLimit);
}

void gateway_runner_correlates_events() {
    dme::OrderBook book({90, 110, 1, 32});
    dme::SpscQueue<dme::GatewayRequest> input(16);
    dme::SpscQueue<dme::GatewayResponse> output(32);
    std::atomic<bool> stop{false};
    dme::GatewayEngineRunner runner(book, input, output);
    std::thread core([&] { runner.run(stop); });
    CHECK(input.try_push({77, 9, order(0, 123, dme::Side::Sell, 100, 5)}));
    std::vector<dme::GatewayResponse> received;
    while (received.size() < 2) {
        dme::GatewayResponse response{};
        if (output.try_pop(response)) received.push_back(response);
    }
    stop.store(true, std::memory_order_release);
    core.join();
    CHECK(received[0].session_id == 77 && received[0].client_sequence == 9);
    CHECK(received[0].event.sequence == 1 && received[0].event.type == dme::EventType::Accepted);
    CHECK(received[1].event.type == dme::EventType::Rested);
}

} // namespace

int main() {
    struct Test { std::string_view name; void (*run)(); };
    const std::vector<Test> tests{
        {"price_time_priority", price_time_priority},
        {"best_price_and_partial_fill", best_price_and_partial_fill},
        {"cancel_and_replace_loses_priority", cancel_and_replace_loses_priority},
        {"immediate_and_fill_or_kill", immediate_and_fill_or_kill},
        {"rejects_gaps_and_duplicates", rejects_gaps_and_duplicates},
        {"executable_order_is_allowed_when_book_is_full", executable_order_is_allowed_when_book_is_full},
        {"snapshot_round_trip", snapshot_round_trip},
        {"journal_round_trip", journal_round_trip},
        {"journal_rejects_bad_header_and_detects_torn_tail", journal_rejects_bad_header_and_detects_torn_tail},
        {"queue_orders_cross_thread", queue_orders_cross_thread},
        {"engine_runner_preserves_output", engine_runner_preserves_output},
        {"protocol_round_trips_and_handles_fragmentation", protocol_round_trips_and_handles_fragmentation},
        {"protocol_rejects_malformed_frames", protocol_rejects_malformed_frames},
        {"session_sequence_and_risk_validation", session_sequence_and_risk_validation},
        {"gateway_runner_correlates_events", gateway_runner_correlates_events}
    };
    std::size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << '/' << tests.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
