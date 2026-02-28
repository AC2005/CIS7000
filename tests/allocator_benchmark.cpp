#include "virtual_memory_manager.h"
#include "slab_allocator.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <algorithm>
#include <cassert>
#include "signal_handler.h"
// -------------------------------------------------------
// Timing
// -------------------------------------------------------

static inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
}

// -------------------------------------------------------
// Histogram
// -------------------------------------------------------

struct Histogram {
    static constexpr size_t NUM_BUCKETS = 64;
    uint64_t buckets[NUM_BUCKETS] = {};
    uint64_t count = 0;
    uint64_t total = 0;
    uint64_t min_val = UINT64_MAX;
    uint64_t max_val = 0;

    void record(uint64_t ns) {
        count++;
        total += ns;
        min_val = std::min(min_val, ns);
        max_val = std::max(max_val, ns);
        int bucket = 0;
        if (ns > 0) bucket = 63 - __builtin_clzll(ns);
        buckets[std::min(bucket, (int)NUM_BUCKETS - 1)]++;
    }

    uint64_t percentile(double p) const {
        uint64_t target = (uint64_t)(count * p / 100.0);
        uint64_t cumulative = 0;
        for (size_t i = 0; i < NUM_BUCKETS; i++) {
            cumulative += buckets[i];
            if (cumulative >= target) return 1ULL << i;
        }
        return max_val;
    }

    void print(const char* label) const {
        printf("%-35s  avg: %5.1f ns  min: %4zu ns  p50: %4zu ns  p99: %4zu ns  p99.9: %5zu ns  max: %6zu ns\n",
               label,
               (double)total / count,
               (size_t)min_val,
               (size_t)percentile(50),
               (size_t)percentile(99),
               (size_t)percentile(99.9),
               (size_t)max_val);
    }
};

// -------------------------------------------------------
// Order struct — 64 bytes
// -------------------------------------------------------

struct Order {
    uint64_t id;
    uint64_t price;
    uint64_t quantity;
    uint64_t timestamp;
    char     symbol[16];
    uint64_t flags;
    uint64_t reserved[1];
};
static_assert(sizeof(Order) == 64, "Order must be 64 bytes");

// -------------------------------------------------------
// Benchmark 1: sequential alloc then free
// -------------------------------------------------------

void bench_slab_sequential(SlabAllocator& slab, size_t N,
                            Histogram& alloc_hist, Histogram& free_hist) {
    std::vector<void*> ptrs(N);

    for (size_t i = 0; i < N; i++) {
        uint64_t t0 = now_ns();
        ptrs[i] = slab.allocate();
        uint64_t t1 = now_ns();
        alloc_hist.record(t1 - t0);
        assert(ptrs[i]);
        static_cast<Order*>(ptrs[i])->id = i;  // touch it
    }

    for (size_t i = 0; i < N; i++) {
        uint64_t t0 = now_ns();
        slab.deallocate(ptrs[i]);
        uint64_t t1 = now_ns();
        free_hist.record(t1 - t0);
    }
}

void bench_malloc_sequential(size_t N,
                              Histogram& alloc_hist, Histogram& free_hist) {
    std::vector<void*> ptrs(N);

    for (size_t i = 0; i < N; i++) {
        uint64_t t0 = now_ns();
        ptrs[i] = malloc(sizeof(Order));
        uint64_t t1 = now_ns();
        alloc_hist.record(t1 - t0);
        assert(ptrs[i]);
        static_cast<Order*>(ptrs[i])->id = i;
    }

    for (size_t i = 0; i < N; i++) {
        uint64_t t0 = now_ns();
        free(ptrs[i]);
        uint64_t t1 = now_ns();
        free_hist.record(t1 - t0);
    }
}

// -------------------------------------------------------
// Benchmark 2: interleaved alloc/free (realistic workload)
// -------------------------------------------------------

void bench_slab_interleaved(SlabAllocator& slab, size_t N,
                             Histogram& alloc_hist, Histogram& free_hist) {
    constexpr size_t WINDOW = 1000;
    std::vector<void*> window(WINDOW, nullptr);
    size_t idx = 0;

    for (size_t i = 0; i < N; i++) {
        if (window[idx]) {
            uint64_t t0 = now_ns();
            slab.deallocate(window[idx]);
            uint64_t t1 = now_ns();
            free_hist.record(t1 - t0);
        }
        uint64_t t0 = now_ns();
        window[idx] = slab.allocate();
        uint64_t t1 = now_ns();
        alloc_hist.record(t1 - t0);
        assert(window[idx]);
        static_cast<Order*>(window[idx])->id = i;
        idx = (idx + 1) % WINDOW;
    }

    for (size_t i = 0; i < WINDOW; i++) {
        if (window[i]) slab.deallocate(window[i]);
    }
}

void bench_malloc_interleaved(size_t N,
                               Histogram& alloc_hist, Histogram& free_hist) {
    constexpr size_t WINDOW = 1000;
    std::vector<void*> window(WINDOW, nullptr);
    size_t idx = 0;

    for (size_t i = 0; i < N; i++) {
        if (window[idx]) {
            uint64_t t0 = now_ns();
            free(window[idx]);
            uint64_t t1 = now_ns();
            free_hist.record(t1 - t0);
        }
        uint64_t t0 = now_ns();
        window[idx] = malloc(sizeof(Order));
        uint64_t t1 = now_ns();
        alloc_hist.record(t1 - t0);
        assert(window[idx]);
        static_cast<Order*>(window[idx])->id = i;
        idx = (idx + 1) % WINDOW;
    }

    for (size_t i = 0; i < WINDOW; i++) {
        if (window[i]) free(window[i]);
    }
}

// -------------------------------------------------------
// Benchmark 3: worst case — random alloc/free pattern
// -------------------------------------------------------

void bench_slab_random(SlabAllocator& slab, size_t N,
                        Histogram& alloc_hist, Histogram& free_hist) {
    constexpr size_t POOL = 500;
    std::vector<void*> pool(POOL, nullptr);
    srand(42);

    for (size_t i = 0; i < N; i++) {
        size_t slot = rand() % POOL;
        if (pool[slot]) {
            uint64_t t0 = now_ns();
            slab.deallocate(pool[slot]);
            uint64_t t1 = now_ns();
            free_hist.record(t1 - t0);
            pool[slot] = nullptr;
        } else {
            uint64_t t0 = now_ns();
            pool[slot] = slab.allocate();
            uint64_t t1 = now_ns();
            alloc_hist.record(t1 - t0);
            if (pool[slot]) static_cast<Order*>(pool[slot])->id = i;
        }
    }

    for (size_t i = 0; i < POOL; i++) {
        if (pool[i]) slab.deallocate(pool[i]);
    }
}

void bench_malloc_random(size_t N,
                          Histogram& alloc_hist, Histogram& free_hist) {
    constexpr size_t POOL = 500;
    std::vector<void*> pool(POOL, nullptr);
    srand(42);

    for (size_t i = 0; i < N; i++) {
        size_t slot = rand() % POOL;
        if (pool[slot]) {
            uint64_t t0 = now_ns();
            free(pool[slot]);
            uint64_t t1 = now_ns();
            free_hist.record(t1 - t0);
            pool[slot] = nullptr;
        } else {
            uint64_t t0 = now_ns();
            pool[slot] = malloc(sizeof(Order));
            uint64_t t1 = now_ns();
            alloc_hist.record(t1 - t0);
            if (pool[slot]) static_cast<Order*>(pool[slot])->id = i;
        }
    }

    for (size_t i = 0; i < POOL; i++) {
        if (pool[i]) free(pool[i]);
    }
}

// -------------------------------------------------------
// Main
// -------------------------------------------------------

int main() {
    constexpr size_t MEM_SIZE   = 4ULL * 1024 * 1024 * 1024;
    constexpr size_t SLAB_CAP   = 64ULL * 1024 * 1024;
    constexpr size_t ITERATIONS = 1'000'000;

    VirtualAddressSpace vas(MEM_SIZE);
    RegionManager rm(vas);
    install_signal_handler(&rm);

    SlabAllocator slab(rm, "ORDER_POOL", SLAB_CAP, sizeof(Order));
    if (!slab.is_valid()) {
        fprintf(stderr, "Failed to initialize slab\n");
        return 1;
    }

    printf("Benchmark: slab allocator vs malloc\n");
    printf("Order size: %zu bytes  |  Iterations: %zu\n\n", sizeof(Order), ITERATIONS);

    // warmup — bring caches and branch predictors to steady state
    {
        SlabAllocator warmup(rm, "WARMUP", 4 * 1024 * 1024, sizeof(Order));
        for (size_t i = 0; i < 10000; i++) {
            void* p = warmup.allocate();
            warmup.deallocate(p);
            void* q = malloc(sizeof(Order));
            free(q);
        }
    }

    // ---- sequential ----
    printf("=== Sequential (alloc all, then free all) ===\n");
    {
        Histogram sa, sf, ma, mf;
        bench_slab_sequential(slab, ITERATIONS, sa, sf);
        bench_malloc_sequential(ITERATIONS, ma, mf);
        sa.print("slab   alloc");
        sf.print("slab   free");
        ma.print("malloc alloc");
        mf.print("malloc free");
        printf("speedup alloc p50: %.1fx  p99: %.1fx\n",
               (double)ma.percentile(50) / sa.percentile(50),
               (double)ma.percentile(99) / sa.percentile(99));
    }

    // ---- interleaved ----
    printf("\n=== Interleaved (rolling window of 1000 live allocs) ===\n");
    {
        Histogram sa, sf, ma, mf;
        bench_slab_interleaved(slab, ITERATIONS, sa, sf);
        bench_malloc_interleaved(ITERATIONS, ma, mf);
        sa.print("slab   alloc");
        sf.print("slab   free");
        ma.print("malloc alloc");
        mf.print("malloc free");
        printf("speedup alloc p50: %.1fx  p99: %.1fx\n",
               (double)ma.percentile(50) / sa.percentile(50),
               (double)ma.percentile(99) / sa.percentile(99));
    }

    // ---- random ----
    printf("\n=== Random (random alloc/free pattern) ===\n");
    {
        Histogram sa, sf, ma, mf;
        bench_slab_random(slab, ITERATIONS, sa, sf);
        bench_malloc_random(ITERATIONS, ma, mf);
        sa.print("slab   alloc");
        sf.print("slab   free");
        ma.print("malloc alloc");
        mf.print("malloc free");
        printf("speedup alloc p50: %.1fx  p99: %.1fx\n",
               (double)ma.percentile(50) / sa.percentile(50),
               (double)ma.percentile(99) / sa.percentile(99));
    }

    slab.debug_dump();
    return 0;
}