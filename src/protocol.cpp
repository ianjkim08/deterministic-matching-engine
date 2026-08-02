#include "dme/protocol.hpp"

#include <bit>

namespace dme::protocol {
namespace {

void put_u16(std::byte* output, std::uint16_t value) noexcept {
    output[0] = static_cast<std::byte>(value & 0xffU);
    output[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void put_u32(std::byte* output, std::uint32_t value) noexcept {
    for (unsigned index = 0; index < 4; ++index) {
        output[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void put_u64(std::byte* output, std::uint64_t value) noexcept {
    for (unsigned index = 0; index < 8; ++index) {
        output[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

std::uint16_t get_u16(const std::byte* input) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[0])) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[1]) << 8U);
}

std::uint32_t get_u32(const std::byte* input) noexcept {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[index]))
                 << (index * 8U);
    }
    return value;
}

std::uint64_t get_u64(const std::byte* input) noexcept {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[index]))
                 << (index * 8U);
    }
    return value;
}

bool valid_side(std::uint8_t value) noexcept { return value <= static_cast<std::uint8_t>(Side::Sell); }
bool valid_order_type(std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(OrderType::FillOrKill);
}

MessageType wire_type(CommandType type) noexcept {
    switch (type) {
        case CommandType::New: return MessageType::NewOrder;
        case CommandType::Cancel: return MessageType::CancelOrder;
        case CommandType::Replace: return MessageType::ReplaceOrder;
    }
    return MessageType::NewOrder;
}

void encode_header(std::byte* output, MessageType type, std::uint16_t length,
                   std::uint32_t session, std::uint64_t sequence) noexcept {
    put_u32(output, magic);
    output[4] = static_cast<std::byte>(version);
    output[5] = static_cast<std::byte>(type);
    put_u16(output + 6, length);
    put_u32(output + 8, session);
    put_u32(output + 12, 0);
    put_u64(output + 16, sequence);
}

DecodeStatus validate_header(std::span<const std::byte> input, MessageType& type,
                             std::size_t& frame_length, std::uint32_t& session,
                             std::uint64_t& sequence) noexcept {
    if (input.size() < header_size) return DecodeStatus::NeedMoreData;
    if (get_u32(input.data()) != magic) return DecodeStatus::InvalidMagic;
    if (std::to_integer<std::uint8_t>(input[4]) != version) return DecodeStatus::UnsupportedVersion;
    frame_length = get_u16(input.data() + 6);
    if (frame_length < header_size || frame_length > maximum_frame_size) return DecodeStatus::InvalidLength;
    if (input.size() < frame_length) return DecodeStatus::NeedMoreData;
    type = static_cast<MessageType>(std::to_integer<std::uint8_t>(input[5]));
    session = get_u32(input.data() + 8);
    sequence = get_u64(input.data() + 16);
    return DecodeStatus::Ok;
}

} // namespace

std::size_t request_frame_size(CommandType type) noexcept {
    switch (type) {
        case CommandType::New: return 52;
        case CommandType::Cancel: return 32;
        case CommandType::Replace: return 48;
    }
    return 0;
}

bool encode_request(const Request& request, std::span<std::byte> destination,
                    std::size_t& bytes_written) noexcept {
    bytes_written = 0;
    const std::size_t length = request_frame_size(request.command.type);
    if (length == 0 || destination.size() < length || request.session_id == 0 ||
        request.client_sequence == 0 || request.command.order_id == 0) return false;
    encode_header(destination.data(), wire_type(request.command.type),
                  static_cast<std::uint16_t>(length), request.session_id, request.client_sequence);
    std::byte* payload = destination.data() + header_size;
    if (request.command.type == CommandType::New) {
        payload[0] = static_cast<std::byte>(request.command.side);
        payload[1] = static_cast<std::byte>(request.command.order_type);
        put_u16(payload + 2, 0);
        put_u64(payload + 4, request.command.order_id);
        put_u64(payload + 12, std::bit_cast<std::uint64_t>(request.command.price));
        put_u64(payload + 20, request.command.quantity);
    } else if (request.command.type == CommandType::Cancel) {
        put_u64(payload, request.command.order_id);
    } else {
        put_u64(payload, request.command.order_id);
        put_u64(payload + 8, std::bit_cast<std::uint64_t>(request.command.price));
        put_u64(payload + 16, request.command.quantity);
    }
    bytes_written = length;
    return true;
}

RequestDecodeResult decode_request(std::span<const std::byte> input) noexcept {
    RequestDecodeResult result{};
    MessageType type{};
    std::size_t length{};
    std::uint32_t session{};
    std::uint64_t sequence{};
    result.status = validate_header(input, type, length, session, sequence);
    if (result.status != DecodeStatus::Ok) return result;
    CommandType command_type{};
    switch (type) {
        case MessageType::NewOrder: command_type = CommandType::New; break;
        case MessageType::CancelOrder: command_type = CommandType::Cancel; break;
        case MessageType::ReplaceOrder: command_type = CommandType::Replace; break;
        default: result.status = DecodeStatus::UnknownMessageType; return result;
    }
    if (length != request_frame_size(command_type) || session == 0 || sequence == 0) {
        result.status = DecodeStatus::InvalidField;
        return result;
    }
    const std::byte* payload = input.data() + header_size;
    Command command{};
    command.type = command_type;
    if (command_type == CommandType::New) {
        const auto side = std::to_integer<std::uint8_t>(payload[0]);
        const auto order_type = std::to_integer<std::uint8_t>(payload[1]);
        if (!valid_side(side) || !valid_order_type(order_type)) {
            result.status = DecodeStatus::InvalidField;
            return result;
        }
        command.side = static_cast<Side>(side);
        command.order_type = static_cast<OrderType>(order_type);
        command.order_id = get_u64(payload + 4);
        command.price = std::bit_cast<Price>(get_u64(payload + 12));
        command.quantity = get_u64(payload + 20);
    } else if (command_type == CommandType::Cancel) {
        command.order_id = get_u64(payload);
    } else {
        command.order_type = OrderType::Limit;
        command.order_id = get_u64(payload);
        command.price = std::bit_cast<Price>(get_u64(payload + 8));
        command.quantity = get_u64(payload + 16);
    }
    if (command.order_id == 0) {
        result.status = DecodeStatus::InvalidField;
        return result;
    }
    result.status = DecodeStatus::Ok;
    result.bytes_consumed = length;
    result.request = Request{session, sequence, command};
    return result;
}

bool encode_response(const Response& response, std::span<std::byte> destination,
                     std::size_t& bytes_written) noexcept {
    bytes_written = 0;
    constexpr auto length = response_frame_size();
    if (destination.size() < length || response.session_id == 0 || response.client_sequence == 0) return false;
    encode_header(destination.data(), MessageType::ExecutionEvent, static_cast<std::uint16_t>(length),
                  response.session_id, response.client_sequence);
    std::byte* payload = destination.data() + header_size;
    payload[0] = static_cast<std::byte>(response.event.type);
    payload[1] = static_cast<std::byte>(response.event.aggressor_side);
    payload[2] = static_cast<std::byte>(response.event.reason);
    payload[3] = std::byte{0};
    put_u64(payload + 4, response.event.order_id);
    put_u64(payload + 12, response.event.contra_order_id);
    put_u64(payload + 20, std::bit_cast<std::uint64_t>(response.event.price));
    put_u64(payload + 28, response.event.quantity);
    put_u64(payload + 36, response.event.sequence);
    bytes_written = length;
    return true;
}

ResponseDecodeResult decode_response(std::span<const std::byte> input) noexcept {
    ResponseDecodeResult result{};
    MessageType type{};
    std::size_t length{};
    std::uint32_t session{};
    std::uint64_t sequence{};
    result.status = validate_header(input, type, length, session, sequence);
    if (result.status != DecodeStatus::Ok) return result;
    if (type != MessageType::ExecutionEvent) {
        result.status = DecodeStatus::UnknownMessageType;
        return result;
    }
    if (length != response_frame_size() || session == 0 || sequence == 0) {
        result.status = DecodeStatus::InvalidField;
        return result;
    }
    const std::byte* payload = input.data() + header_size;
    const auto event_type = std::to_integer<std::uint8_t>(payload[0]);
    const auto side = std::to_integer<std::uint8_t>(payload[1]);
    const auto reason = std::to_integer<std::uint8_t>(payload[2]);
    if (event_type > static_cast<std::uint8_t>(EventType::Rested) || !valid_side(side) ||
        reason > static_cast<std::uint8_t>(RejectReason::GatewayBackpressure)) {
        result.status = DecodeStatus::InvalidField;
        return result;
    }
    Event event{};
    event.type = static_cast<EventType>(event_type);
    event.aggressor_side = static_cast<Side>(side);
    event.reason = static_cast<RejectReason>(reason);
    event.order_id = get_u64(payload + 4);
    event.contra_order_id = get_u64(payload + 12);
    event.price = std::bit_cast<Price>(get_u64(payload + 20));
    event.quantity = get_u64(payload + 28);
    event.sequence = get_u64(payload + 36);
    result.status = DecodeStatus::Ok;
    result.bytes_consumed = length;
    result.response = Response{session, sequence, event};
    return result;
}

} // namespace dme::protocol
