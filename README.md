# HFT Trading System — OS Capstone

A high-performance multi-process trading system built in C++17 as an OS capstone project. The system demonstrates production-inspired HFT infrastructure built entirely from scratch: a custom virtual memory manager, slab allocator, lock-free ring buffer, seqlock, shared memory IPC, and a supervisor with crash recovery.

**Platform:** Linux (Docker container), C++17, CMake  
**Sanitizers:** AddressSanitizer, ThreadSanitizer

---

## Architecture

```
market_data_receiver  ──ring buffer──▶  strategy_engine
        │                                      │
        └──────── seqlock (TopOfBook) ─────────┘
        │                                      │
        └──────── heartbeat region ────────────┘
        │                                      │
        └──────── latency region ──────────────┘
                        │
                   supervisor
                  (monitors both,
                   restarts on crash)
```

Three processes communicate exclusively through shared memory — no sockets, no pipes, no kernel round-trips in the hot path.

### Processes

| Process | Role |
|---|---|
| `supervisor` | Creates the heartbeat region, spawns the other two processes, monitors liveness via `pid_alive` + heartbeat staleness, restarts crashed processes |
| `market_data_receiver` | Simulates a market feed: generates `Message` structs, pushes them onto the ring buffer, updates the order book (and seqlock), stamps latency measurements |
| `strategy_engine` | Consumes messages from the ring buffer, reads the top-of-book seqlock, makes trade-signal decisions, detects peer restarts via generation counter |

### Shared Memory Regions

| Name | Type | Creator |
|---|---|---|
| `/trading_ringbuf` | `RingBuffer<1024>` | `market_data_receiver` |
| `/trading_tob` | `Seqlock<TopOfBook>` | `market_data_receiver` |
| `/trading_heartbeat` | `HeartbeatRegion` | `supervisor` |
| `/trading_latency` | `LatencyRegion` | `market_data_receiver` |

---

## Components

### Virtual Memory Manager (`include/virtual_memory_manager.h`)

Two-layer system built on `mmap` / `mprotect`:

- **`VirtualAddressSpace`** — reserves a 4 GB virtual address space with `PROT_NONE` up front. `commit(offset, size)` promotes pages to `PROT_READ|PROT_WRITE` via `mprotect`; `uncommit` revokes them and calls `madvise(MADV_FREE)` to return physical pages to the OS.
- **`RegionManager`** — carves the VAS into named typed regions using a watermark bump allocator. Each region gets guard pages (permanently `PROT_NONE`) on both sides to catch overflows and underflows. Supports up to 16 regions with zero heap allocation (`std::array<Region, 16>`).

### Signal Handler (`include/signal_handler.h`)

Installed on an alternate stack (`sigaltstack`) so it runs even when the main stack overflows. On SIGSEGV, checks whether the faulting address is a guard page:
- **Guard page hit** — prints a diagnostic (region name, fault direction) to stderr using `write()` (async-signal-safe), dumps a backtrace via `backtrace_symbols_fd`, then `_exit(1)`.
- **Non-guard fault** — resets to `SIG_DFL` and re-raises to produce a core dump.

### Slab Allocator (`include/slab_allocator.h`)

Fixed-size object allocator backed by the `RegionManager`. Uses an intrusive free list (next pointer stored in the first 8 bytes of each free slot) for O(1) alloc and free with no heap touches. Doubles committed memory on expansion. Validates pointer alignment and region membership on `deallocate` — aborts on bad pointer rather than silently corrupting the list.

### Ring Buffer (`include/ring_buffer.h`)

Lock-free SPSC ring buffer. Key design points:
- Template parameter `N` must be a power of 2; indices are never-wrapping `uint64_t` with `& (N-1)` masking.
- Head (write index) and tail (read index) sit in separate `alignas(64)` structs with 56 bytes of padding each to prevent false sharing.
- Producer: writes message, then `store(head+1, release)` — ensures data visible before head advances.
- Consumer: `load(head, acquire)` — ensures all writes before the release are visible.

### Seqlock (`include/seqlock.h`)

Reader-writer synchronization without a read-side lock:
- **Writer**: `seq.store(s+1, release)` → write data → `seq.store(s+2, release)`. Odd sequence indicates a write in progress.
- **Reader**: load seq → spin while odd → `atomic_thread_fence(acquire)` → copy data → `atomic_thread_fence(acquire)` → reload seq → retry if changed.
- Sequence counter and data on separate cache lines to avoid writer traffic invalidating reader cache lines.

Seqlock completely eliminates writer starvation that plagues `pthread_rwlock` under heavy read pressure (rwlock write p50: 32 ms vs seqlock write p50: 32–64 ns).

### Shared Memory (`include/shared_memory.h`)

Named constructor pattern: `SharedMemory::create(name, size)` and `SharedMemory::attach(name, size)`.
- `create` calls `shm_unlink` first to clear stale objects from previous crashes, then `shm_open` + `ftruncate` + `mmap`.
- `attach` retries up to 100 × 10 ms to handle the race where the consumer starts before the creator.
- Destructor calls `shm_unlink` only if this instance is the owner (`is_owner` flag). `cleanup()` is idempotent so it's safe to call from both the destructor and a signal handler.

### Order Book (`include/order_book.h`)

Tracks best bid and best ask using `Order` objects allocated from the slab. On each `update(Message&)`, the displaced order is immediately returned to the slab — at most 2 live `Order` objects exist at any time. Writes a `TopOfBook` snapshot to the seqlock whenever best bid or ask changes.

### Heartbeat (`include/heartbeat.h`)

Each process calls `beat()` every 1000 messages, which increments a counter and stores `now_ns()`. The supervisor calls `is_stale(500ms)` in its 100 ms poll loop. A `generation` counter is incremented by the supervisor on each restart; the strategy engine watches the receiver's generation to detect peer restarts and reset `expected_seq = 0`.

### Latency Measurement (`include/latency.h`)

Power-of-2 histogram with nanosecond resolution (`63 - __builtin_clzll(ns)` bucket). Measures:
- Ring buffer push latency (receiver side)
- Order book update latency (receiver side)
- Ring IPC latency: `pop_time - msg.timestamp` (end-to-end across process boundary)
- Top-of-book seqlock read latency
- Full pipeline latency: `tob_read_time - msg.timestamp`

### Supervisor (`src/supervisor.cpp`)

Forks and `exec`s both child processes (receiver first, then engine after a 200 ms delay to let the receiver create shared memory). Main loop runs every 100 ms: reaps zombies, checks `pid_alive` and heartbeat freshness for each process. On failure, increments the generation counter, sends SIGTERM → waits 50 ms → SIGKILL if still alive, then re-spawns. Prints a status table every 5 seconds.

---

## Building

```bash
# Enter the Docker container
docker-compose up -d
docker-compose exec trading-dev bash

# Inside the container
cd /workspace
mkdir -p build && cd build

# Release build (default for running the system)
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Debug build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)

# With AddressSanitizer
cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON ..
make -j$(nproc)

# With ThreadSanitizer
cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON ..
make -j$(nproc)

# Build a single target
make market_data_receiver
make supervisor
make seqlock_test
```

If CMake output goes to the wrong directory, clean and rebuild:
```bash
cd /workspace/build && rm -rf *
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

---

## Running the System

**Important:** all binaries must be run from `/workspace/build` so that the supervisor can resolve `./market_data_receiver` and `./strategy_engine`.

```bash
cd /workspace/build
```

### Option 1 — Supervisor (recommended)

The supervisor manages both child processes and restarts them on crash:

```bash
./supervisor
```

Expected output:
```
supervisor: starting
supervisor: heartbeat region created at 0x...
supervisor: spawned market_data_receiver  pid=...  restart_count=1
supervisor: spawned strategy_engine       pid=...  restart_count=1
supervisor: monitoring processes (heartbeat threshold: 500 ms)

market_data_receiver: creating shared memory regions
market_data_receiver: starting as generation 0

strategy_engine: attaching to shared memory
strategy_engine: running

TRADE SIGNAL #1: ...
```

Stop with `Ctrl-C`. The supervisor forwards SIGTERM to both children and waits for them to exit cleanly. The strategy engine prints the full latency report on shutdown.

### Option 2 — Processes individually

```bash
# Terminal 1
./market_data_receiver &

# Terminal 2
./strategy_engine
```

Note: `market_data_receiver` must start first so it can create the shared memory regions. `strategy_engine` will retry attachment for up to ~1 second.

### Crash recovery demo

With the supervisor running, send SIGKILL to a child process:

```bash
kill -9 $(pgrep market_data_receiver)
```

The supervisor detects the death within 100 ms (via `pid_alive`), increments the generation counter, and re-spawns the process. The strategy engine detects the generation change and resets its sequence counter.

### Cleaning up stale shared memory

If a previous run crashed without cleaning up:

```bash
rm -f /dev/shm/trading_*
```

---

## Tests

Each test is a standalone binary. Run from the build directory:

```bash
cd /workspace/build

./virtual_memory_manager_test
./slab_allocator_test
./ring_buffer_test
./seqlock_test
```

Run with sanitizers:

```bash
# Rebuild with TSan first
cd /workspace/build && rm -rf *
cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON .. && make -j$(nproc)
./seqlock_test
./ring_buffer_test

# Rebuild with ASan
cd /workspace/build && rm -rf *
cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON .. && make -j$(nproc)
./slab_allocator_test
./virtual_memory_manager_test
```

---

## Benchmarks

```bash
cd /workspace/build

# Slab vs malloc allocation benchmark
./allocator_benchmark

# Seqlock vs pthread_rwlock benchmark
./seqlock_benchmark
```

## File Structure

```
include/
  virtual_memory_manager.h   # VirtualAddressSpace, RegionManager
  signal_handler.h           # install_signal_handler()
  slab_allocator.h           # SlabAllocator
  ring_buffer.h              # RingBuffer<N>, Message  (header-only template)
  shared_memory.h            # SharedMemory
  order_book.h               # Order, TopOfBook, OrderBook
  seqlock.h                  # Seqlock<T>  (header-only template)
  heartbeat.h                # Heartbeat, HeartbeatRegion  (header-only)
  latency.h                  # Histogram, LatencyRegion  (header-only)
src/
  virtual_memory_manager.cpp
  signal_handler.cpp
  slab_allocator.cpp
  shared_memory.cpp
  order_book.cpp
  market_data_receiver.cpp   # producer process
  strategy_engine.cpp        # consumer process
  supervisor.cpp             # process monitor / crash recovery
tests/
  virtual_memory_manager_test.cpp
  slab_allocator_test.cpp
  ring_buffer_test.cpp
  seqlock_test.cpp
  allocator_benchmark.cpp
  seqlock_benchmark.cpp
CMakeLists.txt
build/                       # out-of-source build directory
```
