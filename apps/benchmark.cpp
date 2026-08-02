#include "dme/gateway.hpp"
#include "dme/journal.hpp"
#include "dme/order_book.hpp"
#include "dme/protocol.hpp"
#include "dme/spsc_queue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
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
    if (cpu >= CPU_SETSIZE) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(_WIN32)
    if (cpu >= sizeof(DWORD_PTR) * 8U) return false;
    return SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << cpu) != 0;
#else
    static_cast<void>(cpu);
    return false;
#endif
}

std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double value) {
    const auto index = static_cast<std::size_t>(value * static_cast<double>(sorted.size() - 1U));
    return sorted[index];
}

struct Metrics {
    std::string name;
    std::uint64_t operations{};
    double rate{};
    std::uint64_t p50{};
    std::uint64_t p99{};
    std::uint64_t p999{};
    std::uint64_t rejects{};
};

struct CrossWorkload {
    static constexpr std::uint64_t block = 2;
    dme::Command operator()(std::uint64_t index, dme::Sequence sequence) const {
        if ((index & 1U) == 0) {
            return {sequence, dme::CommandType::New, dme::Side::Sell, dme::OrderType::Limit,
                    0, index + 1U, 10'000, 100};
        }
        return {sequence, dme::CommandType::New, dme::Side::Buy,
                dme::OrderType::ImmediateOrCancel, 0, index + 1U, 10'000, 100};
    }
};

struct CancelWorkload {
    static constexpr std::uint64_t block = 2;
    dme::Command operator()(std::uint64_t index, dme::Sequence sequence) const {
        const auto id = (index / 2U) + 1U;
        if ((index & 1U) == 0) {
            return {sequence, dme::CommandType::New, dme::Side::Buy, dme::OrderType::Limit,
                    0, id, 9'999, 100};
        }
        return {sequence, dme::CommandType::Cancel, dme::Side::Buy, dme::OrderType::Limit,
                0, id, 0, 0};
    }
};

struct ReplaceWorkload {
    static constexpr std::uint64_t block = 1;
    dme::Command operator()(std::uint64_t index, dme::Sequence sequence) const {
        return {sequence, dme::CommandType::Replace, dme::Side::Buy, dme::OrderType::Limit,
                0, 1, (index & 1U) == 0 ? 9'999 : 10'000, 100};
    }
};

struct SweepWorkload {
    static constexpr std::uint64_t block = 10;
    dme::Command operator()(std::uint64_t index, dme::Sequence sequence) const {
        const auto position = index % block;
        const auto cycle = index / block;
        if (position < 9) {
            return {sequence, dme::CommandType::New, dme::Side::Sell, dme::OrderType::Limit,
                    0, cycle * 10U + position + 1U, 10'000 + static_cast<dme::Price>(position), 10};
        }
        return {sequence, dme::CommandType::New, dme::Side::Buy, dme::OrderType::Market,
                0, cycle * 10U + 10U, 0, 90};
    }
};

struct MixedWorkload {
    static constexpr std::uint64_t block = 6;
    dme::Command operator()(std::uint64_t index, dme::Sequence sequence) const {
        const auto position = index % block;
        const auto base = (index / block) * 4U + 1U;
        switch (position) {
            case 0: return {sequence, dme::CommandType::New, dme::Side::Buy,
                            dme::OrderType::Limit, 0, base, 9'999, 50};
            case 1: return {sequence, dme::CommandType::New, dme::Side::Sell,
                            dme::OrderType::Limit, 0, base + 1U, 10'001, 50};
            case 2: return {sequence, dme::CommandType::Replace, dme::Side::Buy,
                            dme::OrderType::Limit, 0, base, 10'000, 60};
            case 3: return {sequence, dme::CommandType::New, dme::Side::Sell,
                            dme::OrderType::ImmediateOrCancel, 0, base + 2U, 10'000, 30};
            case 4: return {sequence, dme::CommandType::Cancel, dme::Side::Buy,
                            dme::OrderType::Limit, 0, base, 0, 0};
            default: return {sequence, dme::CommandType::New, dme::Side::Buy,
                              dme::OrderType::Market, 0, base + 3U, 0, 50};
        }
    }
};

template <typename Workload>
Metrics run_core(std::string name, std::uint64_t requested, Workload workload) {
    const std::uint64_t operations = std::max(Workload::block,
        requested - (requested % Workload::block));
    dme::OrderBook book({9'000, 11'000, 1, 65'536});
    std::vector<dme::Event> events;
    events.reserve(64);
    dme::Sequence sequence = 0;
    if constexpr (std::is_same_v<Workload, ReplaceWorkload>) {
        book.process({++sequence, dme::CommandType::New, dme::Side::Buy,
                      dme::OrderType::Limit, 0, 1, 9'999, 100}, events);
    }
    constexpr std::uint64_t warmup_requested = 200'000;
    const std::uint64_t warmup = warmup_requested - warmup_requested % Workload::block;
    for (std::uint64_t index = 0; index < warmup; ++index) {
        events.clear();
        book.process(workload(index, ++sequence), events);
    }
    const auto rejects_before = book.stats().rejected;
    const auto started = Clock::now();
    for (std::uint64_t index = warmup; index < warmup + operations; ++index) {
        events.clear();
        book.process(workload(index, ++sequence), events);
    }
    const auto stopped = Clock::now();
    const double seconds = std::chrono::duration<double>(stopped - started).count();

    constexpr std::uint64_t sample_requested = 100'000;
    const std::uint64_t samples = sample_requested - sample_requested % Workload::block;
    std::vector<std::uint64_t> latency;
    latency.reserve(static_cast<std::size_t>(samples));
    for (std::uint64_t index = warmup + operations;
         index < warmup + operations + samples; ++index) {
        events.clear();
        const auto command = workload(index, ++sequence);
        const auto before = Clock::now();
        book.process(command, events);
        const auto after = Clock::now();
        latency.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count()));
    }
    std::sort(latency.begin(), latency.end());
    return {std::move(name), operations, static_cast<double>(operations) / seconds,
            percentile(latency, 0.50), percentile(latency, 0.99), percentile(latency, 0.999),
            book.stats().rejected - rejects_before};
}

Metrics run_protocol(std::uint64_t operations) {
    std::array<std::byte, dme::protocol::maximum_frame_size> bytes{};
    std::size_t written{};
    std::uint64_t checksum = 0;
    const auto started = Clock::now();
    for (std::uint64_t index = 0; index < operations; ++index) {
        dme::Command command{0, dme::CommandType::New, dme::Side::Buy,
                             dme::OrderType::Limit, 0, index + 1U, 10'000, 100};
        const dme::protocol::Request request{7, index + 1U, command};
        if (!dme::protocol::encode_request(request, bytes, written)) std::abort();
        const auto decoded = dme::protocol::decode_request({bytes.data(), written});
        checksum += decoded.request.command.order_id;
    }
    const auto stopped = Clock::now();
    if (checksum == 0) std::abort();
    const double seconds = std::chrono::duration<double>(stopped - started).count();
    return {"protocol encode+decode", operations, static_cast<double>(operations) / seconds};
}

Metrics run_spsc(std::uint64_t operations, unsigned producer_cpu) {
    dme::SpscQueue<dme::GatewayRequest> queue(65'536);
    std::atomic<bool> ready{false};
    std::atomic<bool> start{false};
    std::uint64_t consumed = 0;
    std::thread consumer([&] {
        static_cast<void>(pin_to_cpu(producer_cpu + 1U));
        ready.store(true, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {}
        dme::GatewayRequest request{};
        while (consumed < operations) {
            if (queue.try_pop(request)) ++consumed;
        }
    });
    while (!ready.load(std::memory_order_acquire)) {}
    const auto started = Clock::now();
    start.store(true, std::memory_order_release);
    for (std::uint64_t index = 0; index < operations; ++index) {
        const dme::GatewayRequest request{7, index + 1U,
            {0, dme::CommandType::New, dme::Side::Buy, dme::OrderType::Limit,
             0, index + 1U, 10'000, 100}};
        while (!queue.try_push(request)) {}
    }
    consumer.join();
    const auto stopped = Clock::now();
    const double seconds = std::chrono::duration<double>(stopped - started).count();
    return {"SPSC cross-thread transfer", operations, static_cast<double>(operations) / seconds};
}

Metrics run_journal(std::uint64_t requested) {
    const std::uint64_t operations = std::min<std::uint64_t>(requested, 1'000'000);
    const auto path = std::filesystem::temp_directory_path() / "dme_benchmark.journal";
    std::filesystem::remove(path);
    const auto started = Clock::now();
    {
        dme::JournalWriter writer(path);
        for (std::uint64_t index = 0; index < operations; ++index) {
            writer.append({index + 1U, dme::CommandType::New, dme::Side::Buy,
                           dme::OrderType::Limit, 0, index + 1U, 10'000, 100});
        }
        writer.flush();
    }
    const auto stopped = Clock::now();
    std::filesystem::remove(path);
    const double seconds = std::chrono::duration<double>(stopped - started).count();
    return {"buffered journal append", operations, static_cast<double>(operations) / seconds};
}

void print(const Metrics& metrics) {
    std::cout << std::left << std::setw(28) << metrics.name
              << std::right << std::setw(14) << std::fixed << std::setprecision(0) << metrics.rate
              << std::setw(12) << metrics.p50 << std::setw(12) << metrics.p99
              << std::setw(12) << metrics.p999 << std::setw(10) << metrics.rejects << '\n';
}

bool numeric(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(),
        [](char character) { return character >= '0' && character <= '9'; });
}

} // namespace

int main(int argc, char** argv) {
    std::string scenario = "all";
    std::uint64_t operations = 2'000'000;
    unsigned cpu = 0;
    if (argc > 1 && numeric(argv[1])) {
        scenario = "cross"; // backwards-compatible historical invocation
        operations = std::strtoull(argv[1], nullptr, 10);
        if (argc > 2) cpu = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10));
    } else {
        if (argc > 1) scenario = argv[1];
        if (argc > 2) operations = std::strtoull(argv[2], nullptr, 10);
        if (argc > 3) cpu = static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10));
    }
    if (operations < 100) {
        std::cerr << "usage: dme_bench [all|cross|cancel|replace|sweep|mixed|protocol|spsc|journal] "
                     "[operations>=100] [logical-cpu]\n";
        return 2;
    }
    const bool pinned = pin_to_cpu(cpu);
    std::cout << "Deterministic Matching Engine benchmark suite\n"
              << "cpu_pinned=" << (pinned ? "yes" : "no")
              << " requested_operations=" << operations << "\n\n"
              << std::left << std::setw(28) << "workload" << std::right << std::setw(14) << "ops/s"
              << std::setw(12) << "p50 ns" << std::setw(12) << "p99 ns"
              << std::setw(12) << "p99.9 ns" << std::setw(10) << "rejects" << '\n';
    const auto run = [&](std::string_view name) { return scenario == "all" || scenario == name; };
    bool matched = false;
    if (run("cross")) { print(run_core("crossing IOC", operations, CrossWorkload{})); matched = true; }
    if (run("cancel")) { print(run_core("insert/cancel churn", operations, CancelWorkload{})); matched = true; }
    if (run("replace")) { print(run_core("replace priority churn", operations, ReplaceWorkload{})); matched = true; }
    if (run("sweep")) { print(run_core("nine-level market sweep", operations, SweepWorkload{})); matched = true; }
    if (run("mixed")) { print(run_core("mixed lifecycle", operations, MixedWorkload{})); matched = true; }
    if (run("protocol")) { print(run_protocol(operations)); matched = true; }
    if (run("spsc")) { print(run_spsc(operations, cpu)); matched = true; }
    if (run("journal")) { print(run_journal(operations)); matched = true; }
    if (!matched) {
        std::cerr << "unknown benchmark scenario: " << scenario << '\n';
        return 2;
    }
    return 0;
}
