#include "dme/gateway.hpp"
#include "dme/protocol.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }

constexpr std::size_t receive_capacity = 64U * 1024U;
constexpr std::size_t transmit_capacity = 256U * 1024U;
constexpr int maximum_events = 128;

struct Connection {
    explicit Connection(int socket_fd) : fd(socket_fd) {}
    int fd{-1};
    std::uint32_t session_id{};
    std::unique_ptr<dme::SessionValidator> validator;
    std::array<std::byte, receive_capacity> receive{};
    std::size_t received{};
    std::array<std::byte, transmit_capacity> transmit{};
    std::size_t transmit_begin{};
    std::size_t transmit_end{};
};

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

class TcpGateway {
public:
    explicit TcpGateway(std::uint16_t port)
        : port_(port), book_({1, 1'000'000'000, 1, 1'000'000}),
          requests_(65'536), responses_(262'144), runner_(book_, requests_, responses_) {}

    ~TcpGateway() {
        stop_.store(true, std::memory_order_release);
        if (engine_thread_.joinable()) engine_thread_.join();
        for (auto& [fd, connection] : connections_) {
            static_cast<void>(connection);
            ::close(fd);
        }
        if (listen_fd_ >= 0) ::close(listen_fd_);
        if (epoll_fd_ >= 0) ::close(epoll_fd_);
    }

    bool initialize() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) return false;
        int enabled = 1;
        static_cast<void>(::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port_);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listen_fd_, SOMAXCONN) != 0) return false;
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) return false;
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = listen_fd_;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) != 0) return false;
        engine_thread_ = std::thread([this] { runner_.run(stop_); });
        return true;
    }

    int run() {
        std::array<epoll_event, maximum_events> events{};
        while (interrupted == 0) {
            drain_engine_responses();
            const int count = ::epoll_wait(epoll_fd_, events.data(), maximum_events, 1);
            if (count < 0) {
                if (errno == EINTR) continue;
                return 1;
            }
            for (int index = 0; index < count; ++index) {
                const auto event = events[static_cast<std::size_t>(index)];
                if (event.data.fd == listen_fd_) {
                    accept_connections();
                    continue;
                }
                auto found = connections_.find(event.data.fd);
                if (found == connections_.end()) continue;
                bool keep = (event.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) == 0;
                if (keep && (event.events & EPOLLIN) != 0) keep = read_requests(*found->second);
                if (keep && (event.events & EPOLLOUT) != 0) keep = flush_output(*found->second);
                if (!keep) close_connection(event.data.fd);
            }
        }
        stop_.store(true, std::memory_order_release);
        engine_thread_.join();
        return 0;
    }

private:
    void accept_connections() {
        while (true) {
            const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                return;
            }
            if (!set_nonblocking(fd)) {
                ::close(fd);
                continue;
            }
            epoll_event event{};
            event.events = EPOLLIN | EPOLLRDHUP;
            event.data.fd = fd;
            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) != 0) {
                ::close(fd);
                continue;
            }
            connections_.emplace(fd, std::make_unique<Connection>(fd));
        }
    }

    void update_interest(Connection& connection) {
        epoll_event event{};
        event.events = EPOLLIN | EPOLLRDHUP;
        if (connection.transmit_begin != connection.transmit_end) event.events |= EPOLLOUT;
        event.data.fd = connection.fd;
        static_cast<void>(::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, connection.fd, &event));
    }

    bool queue_response(Connection& connection, const dme::protocol::Response& response) {
        if (connection.transmit_begin != 0 &&
            transmit_capacity - connection.transmit_end < dme::protocol::response_frame_size()) {
            const std::size_t remaining = connection.transmit_end - connection.transmit_begin;
            std::memmove(connection.transmit.data(), connection.transmit.data() + connection.transmit_begin,
                         remaining);
            connection.transmit_begin = 0;
            connection.transmit_end = remaining;
        }
        const std::size_t available = transmit_capacity - connection.transmit_end;
        std::size_t written{};
        if (!dme::protocol::encode_response(
                response,
                std::span<std::byte>(connection.transmit.data() + connection.transmit_end, available),
                written)) return false;
        connection.transmit_end += written;
        update_interest(connection);
        return true;
    }

    bool reject(Connection& connection, const dme::protocol::Request& request,
                dme::RejectReason reason) {
        const auto event = dme::gateway_rejection(request, reason);
        return queue_response(connection, {request.session_id, request.client_sequence, event});
    }

    bool read_requests(Connection& connection) {
        while (connection.received < receive_capacity) {
            const auto available = receive_capacity - connection.received;
            const auto count = ::recv(connection.fd, connection.receive.data() + connection.received,
                                      available, 0);
            if (count > 0) {
                connection.received += static_cast<std::size_t>(count);
                continue;
            }
            if (count == 0) return false;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return false;
        }
        std::size_t consumed = 0;
        while (consumed < connection.received) {
            const auto input = std::span<const std::byte>(connection.receive.data() + consumed,
                                                          connection.received - consumed);
            const auto decoded = dme::protocol::decode_request(input);
            if (decoded.status == dme::protocol::DecodeStatus::NeedMoreData) break;
            if (decoded.status != dme::protocol::DecodeStatus::Ok) return false;
            const auto& request = decoded.request;
            if (!connection.validator) {
                if (session_to_fd_.contains(request.session_id)) return false;
                connection.session_id = request.session_id;
                connection.validator = std::make_unique<dme::SessionValidator>(request.session_id);
                session_to_fd_.emplace(request.session_id, connection.fd);
            }
            const auto reason = connection.validator->validate_and_advance(request);
            if (reason != dme::RejectReason::None) {
                if (!reject(connection, request, reason)) return false;
            } else {
                const dme::GatewayRequest engine_request{request.session_id, request.client_sequence,
                                                         request.command};
                if (!requests_.try_push(engine_request) &&
                    !reject(connection, request, dme::RejectReason::GatewayBackpressure)) return false;
            }
            consumed += decoded.bytes_consumed;
        }
        if (consumed != 0) {
            const std::size_t remaining = connection.received - consumed;
            std::memmove(connection.receive.data(), connection.receive.data() + consumed, remaining);
            connection.received = remaining;
        }
        // A full buffer is only fatal when decoding could not consume a frame.
        // Checking before the decode loop would incorrectly disconnect a peer
        // whose final recv happened to fill the buffer exactly.
        if (connection.received == receive_capacity) return false;
        return true;
    }

    bool flush_output(Connection& connection) {
        while (connection.transmit_begin != connection.transmit_end) {
            const auto count = ::send(connection.fd,
                                      connection.transmit.data() + connection.transmit_begin,
                                      connection.transmit_end - connection.transmit_begin, MSG_NOSIGNAL);
            if (count > 0) {
                connection.transmit_begin += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            return false;
        }
        if (connection.transmit_begin == connection.transmit_end) {
            connection.transmit_begin = 0;
            connection.transmit_end = 0;
        }
        update_interest(connection);
        return true;
    }

    void drain_engine_responses() {
        dme::GatewayResponse response{};
        while (responses_.try_pop(response)) {
            const auto session = session_to_fd_.find(response.session_id);
            if (session == session_to_fd_.end()) continue;
            const auto connection = connections_.find(session->second);
            if (connection == connections_.end() ||
                !queue_response(*connection->second,
                                {response.session_id, response.client_sequence, response.event})) {
                close_connection(session->second);
            }
        }
    }

    void close_connection(int fd) {
        auto found = connections_.find(fd);
        if (found == connections_.end()) return;
        if (found->second->session_id != 0) session_to_fd_.erase(found->second->session_id);
        static_cast<void>(::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr));
        ::close(fd);
        connections_.erase(found);
    }

    std::uint16_t port_{};
    int listen_fd_{-1};
    int epoll_fd_{-1};
    dme::OrderBook book_;
    dme::SpscQueue<dme::GatewayRequest> requests_;
    dme::SpscQueue<dme::GatewayResponse> responses_;
    dme::GatewayEngineRunner runner_;
    std::atomic<bool> stop_{false};
    std::thread engine_thread_;
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    std::unordered_map<std::uint32_t, int> session_to_fd_;
};

} // namespace

int main(int argc, char** argv) {
    const unsigned long requested_port = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 9001UL;
    if (requested_port == 0 || requested_port > 65'535UL) {
        std::cerr << "usage: dme_gateway [port]\n";
        return 2;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    TcpGateway gateway(static_cast<std::uint16_t>(requested_port));
    if (!gateway.initialize()) {
        std::cerr << "gateway initialization failed: " << std::strerror(errno) << '\n';
        return 1;
    }
    std::cout << "dme_gateway listening on 0.0.0.0:" << requested_port << '\n';
    return gateway.run();
}
