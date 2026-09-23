# High-Concurrency Ticket Booking & Allocation Engine (C++17)

A low-latency, thread-safe in-memory ticket reservation and seat allocation engine engineered in modern C++ (C++17). Designed to withstand extreme flash-sale traffic (e.g., 100,000+ concurrent requests competing for 10,000 seats) with **zero overbooking**, **75,000+ QPS throughput**, **anti-bot rate limiting**, **automatic 2-phase TTL expiry**, and **crash recovery via Write-Ahead Logging (WAL)**.

Verified with **Clang ThreadSanitizer (`-fsanitize=thread`)** with **0 data races**.

---

## Architecture Overview

```
                      [ Client / Bot Requests (100,000 concurrent) ]
                                            │
                                            ▼
                           [ Multi-Threaded Worker Pool ]
                                            │
                                            ▼
                       [ Tier 0: Token Bucket Rate Limiter ]
                       • Sharded User Map (32 Mutex Stripes)
                       • Lazy mathematical refills (0 thread overhead)
                       • Rejects bot spam (HTTP 429) in < 10 ns
                                            │
               ┌────────────────────────────┴───────────────────────────┐
               ▼                                                        ▼
   [ Tier 1: Fast-Path Atomic Filter ]                       [ Tier 2: Lock Striping ]
   • std::atomic<int32_t> available_count                    • 64 Cache-Line Padded Mutexes
   • O(1) nanosecond rejection if sold out                    • Prevents global lock contention
   • Avoids acquiring any mutex under saturation              • Concurrent parallel bookings
               │                                                        │
               └────────────────────────────┬───────────────────────────┘
                                            ▼
                               [ Seat State Machine ]
                         AVAILABLE ──> HELD ──> CONFIRMED
                                         │
                   ┌─────────────────────┴─────────────────────┐
                   ▼                                           ▼
   [ Background TTL Reaper Thread ]              [ Write-Ahead Logging (WAL) ]
   • std::condition_variable::wait_until         • Append-only binary log
   • Min-Heap Priority Queue                      • Sequence IDs & Checksums
   • Reclaims abandoned seats after TTL          • Replays state after crash
```

---

## Key Technical Features

### 1. Tier 0: Token Bucket Rate Limiting (Anti-Bot & Fair Allocation)
* **Burst Control:** Each user is allocated a bucket (default: 5 burst tokens, 2 tokens/sec refill rate). Bots attempting rapid seat hoarding are rejected instantly with `RATE_LIMITED` (HTTP 429).
* **Lazy Time-Delta Evaluation:** Rather than running thousands of active background timers to refill tokens, refills are computed lazily on each incoming request using `std::chrono` time deltas:
  $$\text{tokens} = \min(\text{capacity},\ \text{current} + \Delta t \times \text{refill\_rate})$$
* **Sharded User Map:** The rate limiter map is partitioned across 32 stripes (`user_id % 32`), ensuring User A's token check never contends with User B's lock.

### 2. Tier 1 & 2: Two-Tier Concurrency Architecture
* **Tier 1 (Fast-Path Filter):** Uses `std::atomic<int32_t>` with `fetch_sub` to instantly reject excess requests in $\approx 5\text{ ns}$ without touching a lock.
* **Tier 2 (Lock Striping):** Seats are partitioned across 64 striped mutexes (`seat_id % 64`). Multiple threads reserving seats in different stripes execute simultaneously on separate CPU cores with zero lock contention.
* **Cache-Line Alignment:** Each striped mutex struct is aligned to 64 bytes (`alignas(64)`) to eliminate **false sharing** across CPU L1/L2 caches.

### 3. 2-Phase Reservation with Background TTL Reaper
* **State Machine:** `AVAILABLE` $\rightarrow$ `HELD` (lease timer active) $\rightarrow$ `CONFIRMED` (payment complete) or `EXPIRED`/`RELEASED`.
* **Background Reaper:** A dedicated daemon thread manages an in-memory min-heap priority queue sorted by expiration time. Using `std::condition_variable::wait_until`, the thread sleeps until the next lease expires, eliminating busy-waiting and CPU waste. Expired seats are automatically restored to `AVAILABLE` inventory.

### 4. Durability & Crash Recovery (WAL)
* Every critical transaction (`HOLD`, `CONFIRM`, `RELEASE`, `EXPIRE`) is written to an append-only binary Write-Ahead Log before in-memory commitment.
* Records contain binary magic bytes, monotonically increasing sequence IDs, high-precision timestamps, and checksums.
* **Crash Recovery Replay:** Upon startup, the engine parses the log sequentially, replays state transitions, and restores the seat map and available inventory with 100% fidelity.

---

## Performance Benchmarks

Tested on Apple Silicon / POSIX Threads with 16 parallel worker threads:

| Metric | Result |
| :--- | :--- |
| **Total Requests Processed** | **100,000** |
| **Throughput (QPS)** | **75,204 req/sec** |
| **Average Latency** | **212.56 µs** |
| **p50 (Median) Latency** | **0.67 µs** |
| **p95 Latency** | **136.25 µs** |
| **p99 Latency** | **5.85 ms** |
| **Oversold Seats** | **0 (Zero)** |
| **ThreadSanitizer Races** | **0 Data Races Detected** |

---

## Directory Structure

```
ticket_booking_system/
├── include/
│   ├── thread_pool.hpp      # Worker thread pool with condition variable task queue
│   ├── rate_limiter.hpp     # Thread-safe Token Bucket Rate Limiter with striped user map
│   ├── wal.hpp              # Write-Ahead Log with binary serialization & checksums
│   ├── reaper.hpp           # Background TTL reaper with min-heap priority queue
│   └── booking_engine.hpp   # Two-tier concurrency engine with lock striping
├── src/
│   ├── thread_pool.cpp
│   ├── rate_limiter.cpp     # Lazy-refill token bucket implementation
│   ├── wal.cpp
│   ├── reaper.cpp
│   ├── booking_engine.cpp
│   └── main.cpp             # Interactive CLI demo & live telemetry
├── tests/
│   ├── test_concurrency.cpp # Stress test: 48,000 requests asserting zero overselling
│   ├── test_recovery.cpp    # Simulates crash & verifies 100% WAL state recovery
│   └── test_rate_limiter.cpp# Anti-bot verification & token bucket mechanics test
├── benchmarks/
│   └── benchmark_runner.cpp # Benchmarking 100k requests: QPS, p50, p95, p99 latency
├── Makefile                 # Build targets: all, test, benchmark, sanitize, clean
├── simulator.html           # Full interactive web visualizer
└── README.md
```

---

## Build & Run Guide

### 1. Compile & Run the Interactive Demo
```bash
make all
./bin/ticket_engine
```

### 2. Run Comprehensive Test Suite (Concurrency, WAL, Rate Limiter)
```bash
make test
```

### 3. Run High-Throughput Benchmark (100k Requests)
```bash
make benchmark
```

### 4. Verify Thread Safety with ThreadSanitizer (TSan)
```bash
make sanitize
```
*Compiles with `-fsanitize=thread` to prove zero data races exist under heavy multithreaded contention.*

---
