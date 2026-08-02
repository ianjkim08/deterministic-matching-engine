#include "dme/gateway.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct TestFailure : std::runtime_error { using std::runtime_error::runtime_error; };

#define CHECK(expression) do { if (!(expression)) throw TestFailure(#expression); } while (false)

bool wait_for(int fd, short events) {
    pollfd descriptor{fd, events, 0};
    int result{};
    do {
        result = ::poll(&descriptor, 1, 2'000);
    } while (result < 0 && errno == EINTR);
    return result == 1 && (descriptor.revents & events) != 0;
}

void send_all(int fd, std::span<const std::byte> bytes) {
    std::size_t offset{};
    while (offset < bytes.size()) {
        CHECK(wait_for(fd, POLLOUT));
        const auto count = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        CHECK(count > 0);
        offset += static_cast<std::size_t>(count);
    }
}

void fragmented_socket_round_trip() {
    std::array<int, 2> sockets{-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) == 0);

    dme::OrderBook book({90, 110, 1, 32});
    dme::SpscQueue<dme::GatewayRequest> requests(16);
    dme::SpscQueue<dme::GatewayResponse> responses(32);
    std::atomic<bool> stop{false};
    dme::GatewayEngineRunner runner(book, requests, responses);
    std::thread engine([&] { runner.run(stop); });

    std::thread server([&] {
        std::array<std::byte, dme::protocol::maximum_frame_size> receive{};
        std::size_t received{};
        dme::protocol::Request request{};
        for (;;) {
            CHECK(wait_for(sockets[1], POLLIN));
            const auto count = ::recv(sockets[1], receive.data() + received,
                                      receive.size() - received, 0);
            CHECK(count > 0);
            received += static_cast<std::size_t>(count);
            const auto decoded = dme::protocol::decode_request({receive.data(), received});
            if (decoded.status == dme::protocol::DecodeStatus::NeedMoreData) continue;
            CHECK(decoded.status == dme::protocol::DecodeStatus::Ok);
            request = decoded.request;
            break;
        }

        dme::SessionValidator validator(request.session_id);
        CHECK(validator.validate_and_advance(request) == dme::RejectReason::None);
        CHECK(requests.try_push({request.session_id, request.client_sequence, request.command}));

        for (std::size_t i = 0; i < 2; ++i) {
            dme::GatewayResponse response{};
            while (!responses.try_pop(response)) std::this_thread::yield();
            std::array<std::byte, dme::protocol::maximum_frame_size> encoded{};
            std::size_t written{};
            CHECK(dme::protocol::encode_response(
                {response.session_id, response.client_sequence, response.event}, encoded, written));
            const auto midpoint = written / 2U;
            send_all(sockets[1], {encoded.data(), midpoint});
            send_all(sockets[1], {encoded.data() + midpoint, written - midpoint});
        }
    });

    const dme::Command command{0, dme::CommandType::New, dme::Side::Buy,
                               dme::OrderType::Limit, 0, 91, 100, 7};
    std::array<std::byte, dme::protocol::maximum_frame_size> encoded{};
    std::size_t written{};
    CHECK(dme::protocol::encode_request({12, 1, command}, encoded, written));
    send_all(sockets[0], {encoded.data(), 1});
    send_all(sockets[0], {encoded.data() + 1, 3});
    send_all(sockets[0], {encoded.data() + 4, written - 4U});

    std::vector<dme::protocol::Response> decoded_responses;
    std::array<std::byte, dme::protocol::maximum_frame_size * 2U> receive{};
    std::size_t received{};
    while (decoded_responses.size() < 2U) {
        CHECK(wait_for(sockets[0], POLLIN));
        const auto count = ::recv(sockets[0], receive.data() + received,
                                  receive.size() - received, 0);
        CHECK(count > 0);
        received += static_cast<std::size_t>(count);
        std::size_t consumed{};
        while (consumed < received) {
            const auto decoded = dme::protocol::decode_response(
                {receive.data() + consumed, received - consumed});
            if (decoded.status == dme::protocol::DecodeStatus::NeedMoreData) break;
            CHECK(decoded.status == dme::protocol::DecodeStatus::Ok);
            decoded_responses.push_back(decoded.response);
            consumed += decoded.bytes_consumed;
        }
        if (consumed != 0U) {
            const auto remaining = received - consumed;
            std::move(receive.begin() + static_cast<std::ptrdiff_t>(consumed),
                      receive.begin() + static_cast<std::ptrdiff_t>(received), receive.begin());
            received = remaining;
        }
    }

    server.join();
    stop.store(true, std::memory_order_release);
    engine.join();
    ::close(sockets[0]);
    ::close(sockets[1]);

    CHECK(decoded_responses[0].session_id == 12);
    CHECK(decoded_responses[0].client_sequence == 1);
    CHECK(decoded_responses[0].event.type == dme::EventType::Accepted);
    CHECK(decoded_responses[0].event.sequence == 1);
    CHECK(decoded_responses[1].event.type == dme::EventType::Rested);
    CHECK(decoded_responses[1].event.order_id == 91);
}

} // namespace

int main() {
    try {
        fragmented_socket_round_trip();
        std::cout << "PASS fragmented_socket_round_trip\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL fragmented_socket_round_trip: " << error.what() << '\n';
        return 1;
    }
}
