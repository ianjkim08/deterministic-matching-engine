#pragma once

#include "dme/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace dme::protocol {

inline constexpr std::uint32_t magic = 0x31454d44U; // "DME1" on the wire
inline constexpr std::uint8_t version = 1;
inline constexpr std::size_t header_size = 24;
inline constexpr std::size_t maximum_frame_size = 80;

enum class MessageType : std::uint8_t {
    NewOrder = 1,
    CancelOrder = 2,
    ReplaceOrder = 3,
    ExecutionEvent = 128
};

enum class DecodeStatus : std::uint8_t {
    Ok,
    NeedMoreData,
    InvalidMagic,
    UnsupportedVersion,
    InvalidLength,
    UnknownMessageType,
    InvalidField
};

struct Request {
    std::uint32_t session_id{};
    std::uint64_t client_sequence{};
    Command command{}; // engine sequence is assigned by the single-writer runner
};

struct Response {
    std::uint32_t session_id{};
    std::uint64_t client_sequence{};
    Event event{};
};

struct RequestDecodeResult {
    DecodeStatus status{DecodeStatus::NeedMoreData};
    std::size_t bytes_consumed{};
    Request request{};
};

struct ResponseDecodeResult {
    DecodeStatus status{DecodeStatus::NeedMoreData};
    std::size_t bytes_consumed{};
    Response response{};
};

[[nodiscard]] std::size_t request_frame_size(CommandType type) noexcept;
[[nodiscard]] constexpr std::size_t response_frame_size() noexcept { return 68; }

[[nodiscard]] bool encode_request(const Request& request, std::span<std::byte> destination,
                                  std::size_t& bytes_written) noexcept;
[[nodiscard]] RequestDecodeResult decode_request(std::span<const std::byte> input) noexcept;

[[nodiscard]] bool encode_response(const Response& response, std::span<std::byte> destination,
                                   std::size_t& bytes_written) noexcept;
[[nodiscard]] ResponseDecodeResult decode_response(std::span<const std::byte> input) noexcept;

} // namespace dme::protocol
