#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace dme {

template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(std::size_t minimum_capacity)
        : buffer_(storage_capacity(minimum_capacity)), mask_(buffer_.size() - 1U) {}

    [[nodiscard]] bool try_push(const T& value) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto next = (tail + 1U) & mask_;
        if (next == head_.load(std::memory_order_acquire)) return false;
        buffer_[tail] = value;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& value) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        value = buffer_[head];
        head_.store((head + 1U) & mask_, std::memory_order_release);
        return true;
    }

private:
    static std::size_t storage_capacity(std::size_t minimum) {
        constexpr auto digits = std::numeric_limits<std::size_t>::digits;
        constexpr std::size_t largest_power_of_two = std::size_t{1} << (digits - 1U);
        if (minimum == 0 || minimum >= largest_power_of_two) {
            throw std::invalid_argument("invalid SPSC queue capacity");
        }
        return std::bit_ceil(minimum + 1U);
    }

    static constexpr std::size_t cache_line = 64;
    std::vector<T> buffer_;
    std::size_t mask_{};
    alignas(cache_line) std::atomic<std::size_t> head_{0};
    alignas(cache_line) std::atomic<std::size_t> tail_{0};
};

} // namespace dme
