# Deterministic Matching Engine

A dependency-free C++20 limit-order-book, matching engine, and Linux TCP gateway
built for deterministic behavior, bounded memory use, and reproducible measurement.

This is an educational systems project, not a production exchange. It includes an
end-to-end reference path, but its reported latency numbers remain component
microbenchmarks rather than network round-trip measurements.

## Highlights

- Limit, market, immediate-or-cancel (IOC), and fill-or-kill (FOK) orders
- Cancel and replace, with replacement losing queue priority
- Price-time priority, partial fills, and execution at the resting order's price
- Integer prices and quantities; floating-point values never enter book state
- Preallocated order arena and fixed-capacity, open-addressed order-ID index
- Intrusive FIFO queues and contiguous price ladders
- Active-price bitmaps for bounded best-bid/best-ask discovery
- Strict command sequencing and explicit rejection reasons
- Versioned little-endian binary protocol with streaming frame validation
- Linux `epoll` TCP gateway with bounded receive/transmit buffers
- Per-session sequence enforcement, pre-trade quantity/price/notional limits, and
  explicit input-queue backpressure rejection
- Cache-line-separated SPSC queues with explicit output backpressure
- Checksummed command journal, torn-record detection, snapshots, and replay
- CMake builds on Linux and Windows with no third-party runtime dependencies

Book storage, price levels, and the order-ID index do not allocate after
construction. Event storage is caller-owned: reserve enough output capacity for the
largest expected sweep if heap allocation is prohibited on the calling path.

## Architecture

```text
 TCP clients
     |
 epoll gateway -- decode / session sequence / risk
     |
 bounded SPSC request queue
     |
 single-writer runner ----> optional command journal
     |
 matching engine
  /      |       \
 ladders order-ID  deterministic events
                    |
              bounded SPSC response queue
                    |
          encode / per-session TCP routing
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
./build/dme_bench all 2000000 2
./build/dme_gateway 9001
```

Windows from a Visual Studio developer shell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\dme_bench.exe all 2000000 2
```

The benchmark arguments are `[scenario] [operation-count] [logical-cpu]`, where
`scenario` is `all`, `cross`, `cancel`, `replace`, `sweep`, `mixed`, `protocol`,
`spsc`, or `journal`. The historical numeric-only form still runs `cross`.

`dme_gateway` is Linux-only and listens on all interfaces. The first valid frame on
a connection binds its nonzero session ID; client sequences then begin at 1 and
must be contiguous. See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the exact wire
layout and rejection behavior.

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

The benchmark suite now measures crossing IOC, insert/cancel churn, replacement,
nine-level market sweeps, mixed lifecycle traffic, protocol codec throughput,
cross-thread SPSC transfer, and buffered journal append. Workload construction,
startup allocation, and warmup are outside timed engine loops. The latency pass
includes `steady_clock` measurement cost.

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
- cross-thread SPSC ordering and runner event delivery;
- binary protocol round trips, fragmented input, and malformed-field rejection;
- session sequencing, overflow-safe risk limits, and gateway event correlation;
- 50,000-command differential comparison against an independent reference book;
- 100,000 randomized malformed protocol frames;
- fragmented bidirectional OS stream transport on Linux.

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
apps/          benchmark, replay, and Linux TCP gateway executables
tests/         focused, differential, fuzz-style, and socket integration tests
docs/          binary protocol specification
scripts/       reproducible Linux benchmark helper
```

## Known boundaries

- One `OrderBook` represents one configured price ladder; multi-instrument routing
  belongs above the core.
- FOK validation may walk multiple eligible price levels.
- The fixed order capacity and price range are chosen at construction.
- The journal checksum detects accidental corruption; it is not cryptographic.
- The TCP gateway has no TLS, authentication, authorization, DDoS controls, kernel
  bypass, multi-process failover, or production observability.
- Gateway risk limits are fixed defaults in the reference executable; a deployment
  would load per-account policy from controlled configuration.
- Published latency percentiles are in-process measurements, not wire-to-wire or
  durable-ack latency.

## Contributing and license

See [CONTRIBUTING.md](CONTRIBUTING.md) before submitting changes. Security-sensitive
reports should follow [SECURITY.md](SECURITY.md).

Released under the [MIT License](LICENSE).
