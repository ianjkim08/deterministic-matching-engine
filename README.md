# Deterministic Matching Engine

A dependency-free C++20 limit-order-book and matching-engine core built for
deterministic behavior, bounded memory use, and reproducible latency measurement.

This is an educational systems project, not a production exchange. Its benchmark
measures the in-process matching path only; it does not include a network gateway,
wire decoding, pre-trade risk, durable media synchronization, or response encoding.

## Highlights

- Limit, market, immediate-or-cancel (IOC), and fill-or-kill (FOK) orders
- Cancel and replace, with replacement losing queue priority
- Price-time priority, partial fills, and execution at the resting order's price
- Integer prices and quantities; floating-point values never enter book state
- Preallocated order arena and fixed-capacity, open-addressed order-ID index
- Intrusive FIFO queues and contiguous price ladders
- Active-price bitmaps for bounded best-bid/best-ask discovery
- Strict command sequencing and explicit rejection reasons
- Cache-line-separated SPSC queues with explicit output backpressure
- Checksummed command journal, torn-record detection, snapshots, and replay
- CMake builds on Linux and Windows with no third-party runtime dependencies

Book storage, price levels, and the order-ID index do not allocate after
construction. Event storage is caller-owned: reserve enough output capacity for the
largest expected sweep if heap allocation is prohibited on the calling path.

## Architecture

```text
 gateway / command producer
            |
       SPSC command queue
            |
     single-writer runner ----> command journal
            |
       matching engine
       /      |       \
  bid/ask   order-ID   event generation
  ladders    index            |
                         SPSC event queue
                                |
                        publisher / consumer
```

The book is deliberately single-writer. Gateway and publication work can run on
separate threads, while instruments can be sharded across independent book-owning
cores.

## Quick start

Requirements:

- CMake 3.20 or newer
- A C++20 compiler
- Ninja is optional

Linux:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/dme_bench 10000000 2
```

Windows from a Visual Studio developer shell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\dme_bench.exe 10000000 2
```

The benchmark arguments are `[operation-count] [logical-cpu]`. It reports whether
thread affinity succeeded.

## Minimal API example

```cpp
#include <dme/order_book.hpp>

#include <vector>

int main() {
    dme::OrderBook book({9'000, 11'000, 1, 65'536});
    std::vector<dme::Event> events;
    events.reserve(64);

    const dme::Command sell{
        1, dme::CommandType::New, dme::Side::Sell,
        dme::OrderType::Limit, 0, 1001, 10'000, 25
    };
    book.process(sell, events);
}
```

Each accepted command must have `sequence == last_sequence + 1`. Events are
appended to the supplied vector and remain in deterministic generation order.

## Verified performance

Nine CPU-pinned, 20-million-operation trials on an Intel Core i7-10700 measured a
**17.70M operations/second median**, with a **13.55M to 20.77M** observed range. The
conservative worst-trial latency values were **100 ns p50, 400 ns p99, and 600 ns
p99.9**, with zero benchmark rejects.

See [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) for the exact environment, flags,
workload, all repeat trials, and interpretation limits.

The benchmark alternates a resting limit sell with a crossing IOC buy. It exercises
insertion, best-price discovery, matching, removal, order-ID maintenance, and event
generation. Workload construction, startup allocation, and warmup are outside the
timed throughput loop. The latency pass includes `steady_clock` measurement cost.

These values are **matching-core microbenchmark results**, not network-to-network
order latency. Results on another CPU, compiler, operating system, power policy, or
workload will differ.

## Persistence and recovery

`JournalWriter` writes fixed-size checksummed command records. `replay_journal`
stops at the first incomplete or checksum-invalid record and reports the last valid
byte offset. Existing journal headers are validated before new records are appended.

Snapshots are written to a temporary file and then replaced at the destination so
a partially written snapshot is not exposed as current. Replacement guarantees
ultimately depend on the local filesystem.

Setting `synchronous=true` flushes the C++ stream after each journal record. It does
**not** guarantee that every record has reached physical media. Production-grade
durability requires a measured platform policy such as `fdatasync`, direct I/O, or
replicated consensus.

Journal and snapshot files are same-build recovery formats. They serialize native
trivially-copyable structures and are not portable wire protocols. A production
format should define byte order, field encoding, and schema migration explicitly.

## Testing

The test suite covers:

- price-time priority, best-price selection, and partial fills;
- cancel/replace behavior and queue-priority loss;
- IOC and FOK semantics;
- capacity, duplicate-ID, and sequence-gap rejection;
- matching executable orders while resting capacity is full;
- snapshot and journal round trips;
- cross-thread SPSC ordering and runner event delivery.

Sanitizer build:

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

GitHub Actions runs release builds and tests on Linux and Windows, plus an
AddressSanitizer/UndefinedBehaviorSanitizer job on Linux.

## Project layout

```text
include/dme/   public headers and data structures
src/           matching and persistence implementations
apps/          benchmark and journal-replay executables
tests/         dependency-free correctness test suite
scripts/       reproducible Linux benchmark helper
```

## Known boundaries

- One `OrderBook` represents one configured price ladder; multi-instrument routing
  belongs above the core.
- FOK validation may walk multiple eligible price levels.
- The fixed order capacity and price range are chosen at construction.
- The journal checksum detects accidental corruption; it is not cryptographic.
- There is no authentication, authorization, network protocol, or pre-trade risk
  layer in this repository.

## Contributing and license

See [CONTRIBUTING.md](CONTRIBUTING.md) before submitting changes. Security-sensitive
reports should follow [SECURITY.md](SECURITY.md).

Released under the [MIT License](LICENSE).
