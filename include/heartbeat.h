#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <time.h>
#include <cstring>

enum class ProcessRole : uint8_t {
    MARKET_DATA_RECEIVER = 0,
    STRATEGY_ENGINE      = 1,
    COUNT                = 2
};

struct Heartbeat {
    alignas(64) std::atomic<uint64_t> counter;
    std::atomic<uint64_t>             timestamp_ns;
    std::atomic<uint64_t>             generation;
    std::atomic<bool>                 alive;
    char                              name[32];
    char                              padding[11];  // pad to 128 bytes total

    void init(const char* process_name) {
        counter.store(0,     std::memory_order_relaxed);
        timestamp_ns.store(0, std::memory_order_relaxed);
        generation.store(0,  std::memory_order_relaxed);
        alive.store(false,   std::memory_order_relaxed);
        strncpy(name, process_name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }

    void beat() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
        counter.fetch_add(1, std::memory_order_relaxed);
        timestamp_ns.store(now, std::memory_order_release);
        alive.store(true, std::memory_order_release);
    }

    void shutdown() {
        alive.store(false, std::memory_order_release);
    }

    // returns true if heartbeat is stale (process may be dead)
    bool is_stale(uint64_t threshold_ns) const {
        if (!alive.load(std::memory_order_acquire)) return true;

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now  = (uint64_t)ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
        uint64_t last = timestamp_ns.load(std::memory_order_acquire);
        return (now - last) > threshold_ns;
    }

    void print() const {
        printf("  [%s]  counter: %zu  generation: %zu  alive: %s\n",
               name,
               (size_t)counter.load(std::memory_order_relaxed),
               (size_t)generation.load(std::memory_order_relaxed),
               alive.load(std::memory_order_relaxed) ? "yes" : "no");
    }
};

// shared memory region containing heartbeats for all processes
struct HeartbeatRegion {
    Heartbeat beats[(size_t)ProcessRole::COUNT];
};