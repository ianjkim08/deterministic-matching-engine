# Verified benchmark results

Measured on 2026-08-01. These are matching-core microbenchmark results, not
network-to-network latency.

## Environment

- CPU: Intel Core i7-10700 @ 2.90 GHz
- OS: Microsoft Windows 10.0.19045
- Compiler driver: Zig 0.16.0 `c++` (LLVM-based C++ toolchain)
- Flags: `-std=c++20 -O3 -DNDEBUG -march=native`
- Affinity: benchmark thread pinned to logical CPU 4
- Timed workload: alternating resting limit sell and crossing IOC buy
- Timed operations per trial: 20,000,000 after 200,000 warmup operations
- Latency sample count: 200,000 per trial; `steady_clock` overhead included
- Journaling and SPSC transport: excluded from this matching-core measurement

## Repeat trials

| Trial | Throughput (ops/s) | p50 | p99 | p99.9 | Rejects |
|---:|---:|---:|---:|---:|---:|
| 1 | 17,695,044 | 100 ns | 200 ns | 500 ns | 0 |
| 2 | 16,703,416 | 100 ns | 400 ns | 500 ns | 0 |
| 3 | 19,053,969 | 100 ns | 200 ns | 500 ns | 0 |
| 4 | 15,366,156 | 100 ns | 300 ns | 500 ns | 0 |
| 5 | 20,769,398 | 100 ns | 300 ns | 500 ns | 0 |
| 6 | 18,599,089 | 100 ns | 300 ns | 500 ns | 0 |
| 7 | 17,310,658 | 100 ns | 400 ns | 500 ns | 0 |
| 8 | 13,552,345 | 100 ns | 200 ns | 400 ns | 0 |
| 9 | 17,935,915 | 100 ns | 400 ns | 600 ns | 0 |

Range: **13.55M-20.77M operations/s**. Median trial throughput: **17.70M
operations/s**. Windows scheduling and clock/power behavior produced substantial
throughput variation even with thread affinity. The clock quantized observed latency
to 100 ns increments, so these figures should not be used to distinguish sub-100-ns
changes. An isolated Linux core with a controlled frequency policy is recommended
for publication-quality comparative measurements.

## Correctness verification

- Optimized suite: 11/11 tests passed
- UndefinedBehaviorSanitizer suite: 11/11 tests passed
- AddressSanitizer + UndefinedBehaviorSanitizer suite: 11/11 tests passed

Coverage includes price-time priority, best-price selection, partial fills,
cancel/replace priority, IOC/FOK behavior, sequence and duplicate rejection,
snapshot replacement and recovery, incompatible/torn journal handling, journal
replay, cross-thread SPSC ordering, and runner output.

## Supported performance statement

The checked-in benchmark supports the statement that the matching-core workload
measured a **17.7M operations/s median**, with **100 ns p50, 400 ns worst-trial p99,
and 600 ns worst-trial p99.9**, across nine pinned 20M-operation trials on the named
system. The full observed throughput range should remain available alongside that
statement.

This must not be described as end-to-end order latency without implementing and
measuring network input, decoding, risk, durable synchronization, response encoding,
and network output.
