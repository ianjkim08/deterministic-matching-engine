#include "dme/order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

bool pin_to_cpu(unsigned cpu) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(_WIN32)
    if (cpu >= sizeof(DWORD_PTR) * 8U) return false;
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << cpu;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#else
    static_cast<void>(cpu);
    return false;
#endif
}

dme::Command make_command(std::uint64_t index, dme::Sequence sequence) {
    if ((index & 1U) == 0) {
        return {sequence, dme::CommandType::New, dme::Side::Sell, dme::OrderType::Limit,
                0, index + 1U, 10'000, 100};
    }
    return {sequence, dme::CommandType::New, dme::Side::Buy,
            dme::OrderType::ImmediateOrCancel, 0, index + 1U, 10'000, 100};
}

std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double p) {
    const auto index = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1U));
    return sorted[index];
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t operations = 5'000'000;
    unsigned cpu = 0;
    if (argc > 1) operations = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) cpu = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10));
    if (operations < 2) {
        std::cerr << "operation count must be at least 2\n";
        return 2;
    }
    operations += operations & 1U; // paired workload

    const bool pinned = pin_to_cpu(cpu);
    dme::OrderBook book({9'000, 11'000, 1, 65'536});
    std::vector<dme::Event> events;
    events.reserve(8);

    constexpr std::uint64_t warmup = 200'000;
    for (std::uint64_t i = 0; i < warmup; ++i) {
        events.clear();
        book.process(make_command(i, i + 1U), events);
    }

    const auto started = Clock::now();
    for (std::uint64_t i = 0; i < operations; ++i) {
        events.clear();
        book.process(make_command(warmup + i, warmup + i + 1U), events);
    }
    const auto stopped = Clock::now();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stopped - started).count();
    const double seconds = static_cast<double>(elapsed_ns) / 1e9;
    const double rate = static_cast<double>(operations) / seconds;

    constexpr std::size_t latency_samples = 200'000;
    std::vector<std::uint64_t> latencies;
    latencies.reserve(latency_samples);
    for (std::size_t i = 0; i < latency_samples; ++i) {
        const auto sequence = warmup + operations + i + 1U;
        const auto command = make_command(warmup + operations + i, sequence);
        events.clear();
        const auto before = Clock::now();
        book.process(command, events);
        const auto after = Clock::now();
        latencies.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
    }
    std::sort(latencies.begin(), latencies.end());

    std::cout << "Deterministic Matching Engine benchmark\n"
              << "workload: alternating resting limit sell / crossing IOC buy\n"
              << "operations: " << operations << '\n'
              << "cpu_pinned: " << (pinned ? "yes" : "no") << '\n'
              << std::fixed << std::setprecision(2)
              << "elapsed_seconds: " << seconds << '\n'
              << "throughput_ops_per_second: " << rate << '\n'
              << "latency_p50_ns: " << percentile(latencies, 0.50) << '\n'
              << "latency_p99_ns: " << percentile(latencies, 0.99) << '\n'
              << "latency_p999_ns: " << percentile(latencies, 0.999) << '\n'
              << "trades_total: " << book.stats().trades << '\n'
              << "rejections_total: " << book.stats().rejected << '\n';
    return book.stats().rejected == 0 ? 0 : 2;
}
