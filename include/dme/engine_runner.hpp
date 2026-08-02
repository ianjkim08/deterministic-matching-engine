#pragma once

#include "dme/journal.hpp"
#include "dme/order_book.hpp"
#include "dme/spsc_queue.hpp"

#include <atomic>
#include <thread>
#include <vector>

namespace dme {

// Single-writer event loop. The gateway is the sole input producer and the
// publisher is the sole output consumer. Backpressure is explicit: events are
// never silently dropped when the output queue is full.
class EngineRunner {
public:
    EngineRunner(OrderBook& book, SpscQueue<Command>& input, SpscQueue<Event>& output,
                 JournalWriter* journal = nullptr)
        : book_(book), input_(input), output_(output), journal_(journal) {}

    void run(const std::atomic<bool>& stop) {
        std::vector<Event> events;
        events.reserve(64);
        Command command{};
        while (!stop.load(std::memory_order_acquire)) {
            if (!input_.try_pop(command)) {
                std::this_thread::yield();
                continue;
            }
            if (journal_ != nullptr) journal_->append(command);
            events.clear();
            book_.process(command, events);
            for (const Event& event : events) {
                while (!output_.try_push(event)) {
                    if (stop.load(std::memory_order_acquire)) return;
                    std::this_thread::yield();
                }
            }
        }
        if (journal_ != nullptr) journal_->flush();
    }

private:
    OrderBook& book_;
    SpscQueue<Command>& input_;
    SpscQueue<Event>& output_;
    JournalWriter* journal_{};
};

} // namespace dme
