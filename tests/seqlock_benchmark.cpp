#define _XOPEN_SOURCE 700
#include "seqlock.h"
#include "order_book.h"
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>
#include <time.h>
#include <cassert>
#include <vector>

static inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
}

// Operations faster than the ~32ns clock tick must be timed in groups.
static constexpr size_t TIMING_BATCH = 1000;

struct Histogram {
    static constexpr size_t NUM_BUCKETS = 64;
    uint64_t buckets[NUM_BUCKETS] = {};
    uint64_t count  = 0;
    uint64_t total  = 0;
    uint64_t min_val = UINT64_MAX;
    uint64_t max_val = 0;

    void record(uint64_t ns) {
        count++;
        total += ns;
        min_val = (ns < min_val) ? ns : min_val;
        max_val = (ns > max_val) ? ns : max_val;
        int b = (ns > 0) ? (63 - __builtin_clzll(ns)) : 0;
        buckets[b < (int)NUM_BUCKETS ? b : (int)NUM_BUCKETS - 1]++;
    }

    uint64_t percentile(double p) const {
        uint64_t target = (uint64_t)(count * p / 100.0);
        uint64_t cum = 0;
        for (size_t i = 0; i < NUM_BUCKETS; i++) {
            cum += buckets[i];
            if (cum >= target) return 1ULL << i;
        }
        return max_val;
    }

    void print(const char* label) const {
        printf("%-40s  avg: %6.1f ns  p50: %5zu ns  p99: %6zu ns  p99.9: %6zu ns  max: %7zu ns  count: %zu\n",
               label,
               (double)total / count,
               (size_t)percentile(50),
               (size_t)percentile(99),
               (size_t)percentile(99.9),
               (size_t)max_val,
               (size_t)count);
    }
};

// seqlock version
static Seqlock<TopOfBook> g_seqlock;

// rwlock version — plain TopOfBook protected by pthread_rwlock
static TopOfBook          g_rwlock_tob{};
static pthread_rwlock_t   g_rwlock = PTHREAD_RWLOCK_INITIALIZER;

static TopOfBook make_tob(uint64_t mid, size_t i) {
    TopOfBook t{};
    t.bid_price    = mid - 5;
    t.bid_quantity = 100 + (i % 50);
    t.ask_price    = mid + 5;
    t.ask_quantity = 100 + (i % 30);
    t.last_update  = (uint64_t)i;
    strncpy(t.symbol, "AAPL", sizeof(t.symbol) - 1);
    return t;
}

struct BenchConfig {
    size_t num_readers;
    size_t reads_per_writer;   // how many reads per write
    size_t total_writes;
};

struct BenchResult {
    Histogram read_hist;
    Histogram write_hist;
    uint64_t  invalid_reads;
};

// ---- seqlock benchmark ----

BenchResult run_seqlock_bench(const BenchConfig& cfg) {
    BenchResult result{};
    std::atomic<bool>     done{false};
    std::atomic<uint64_t> invalid{0};

    // writer
    std::thread writer([&]() {
        uint64_t mid = 10000;
        for (size_t i = 0; i < cfg.total_writes; i += TIMING_BATCH) {
            size_t batch = std::min(TIMING_BATCH, cfg.total_writes - i);
            uint64_t t0 = now_ns();
            for (size_t j = 0; j < batch; j++) {
                mid += ((i+j) % 3 == 0) ? 1 : ((i+j) % 3 == 1) ? 0 : -1;
                TopOfBook t = make_tob(mid, i + j);
                g_seqlock.write(t);
            }
            uint64_t t1 = now_ns();
            uint64_t per_op = (t1 - t0) / batch;
            for (size_t j = 0; j < batch; j++) result.write_hist.record(per_op);
        }
        done.store(true, std::memory_order_release);
    });

    // readers
    std::vector<std::thread> readers(cfg.num_readers);
    for (size_t r = 0; r < cfg.num_readers; r++) {
        readers[r] = std::thread([&, r]() {
            (void)r;
            while (!done.load(std::memory_order_acquire)) {
                uint64_t t0 = now_ns();
                for (size_t i = 0; i < TIMING_BATCH; i++) {
                    TopOfBook snapshot = g_seqlock.read();
                    if (snapshot.bid_price > 0 && !snapshot.is_valid()) {
                        invalid.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                uint64_t t1 = now_ns();
                uint64_t per_op = (t1 - t0) / TIMING_BATCH;
                for (size_t i = 0; i < TIMING_BATCH; i++) result.read_hist.record(per_op);
            }
        });
    }

    writer.join();
    for (auto& t : readers) t.join();
    result.invalid_reads = invalid.load();
    return result;
}

// ---- rwlock benchmark ----

BenchResult run_rwlock_bench(const BenchConfig& cfg) {
    BenchResult result{};
    std::atomic<bool>     done{false};
    std::atomic<uint64_t> invalid{0};

    std::thread writer([&]() {
        uint64_t mid = 10000;
        for (size_t i = 0; i < cfg.total_writes; i += TIMING_BATCH) {
            size_t batch = std::min(TIMING_BATCH, cfg.total_writes - i);
            uint64_t t0 = now_ns();
            for (size_t j = 0; j < batch; j++) {
                mid += ((i+j) % 3 == 0) ? 1 : ((i+j) % 3 == 1) ? 0 : -1;
                TopOfBook t = make_tob(mid, i + j);
                pthread_rwlock_wrlock(&g_rwlock);
                g_rwlock_tob = t;
                pthread_rwlock_unlock(&g_rwlock);
            }
            uint64_t t1 = now_ns();
            uint64_t per_op = (t1 - t0) / batch;
            for (size_t j = 0; j < batch; j++) result.write_hist.record(per_op);
        }
        done.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers(cfg.num_readers);
    for (size_t r = 0; r < cfg.num_readers; r++) {
        readers[r] = std::thread([&, r]() {
            (void)r;
            while (!done.load(std::memory_order_acquire)) {
                uint64_t t0 = now_ns();
                for (size_t i = 0; i < TIMING_BATCH; i++) {
                    pthread_rwlock_rdlock(&g_rwlock);
                    TopOfBook snapshot = g_rwlock_tob;
                    pthread_rwlock_unlock(&g_rwlock);
                    if (snapshot.bid_price > 0 && !snapshot.is_valid()) {
                        invalid.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                uint64_t t1 = now_ns();
                uint64_t per_op = (t1 - t0) / TIMING_BATCH;
                for (size_t i = 0; i < TIMING_BATCH; i++) result.read_hist.record(per_op);
            }
        });
    }

    writer.join();
    for (auto& t : readers) t.join();
    result.invalid_reads = invalid.load();
    return result;
}

void run_scenario(const char* name, const BenchConfig& cfg) {
    printf("\n=== %s (%zu readers, %zu reads per write) ===\n",
           name, cfg.num_readers, cfg.reads_per_writer);

    // reset shared state
    g_seqlock.reset();
    g_rwlock_tob = TopOfBook{};
    pthread_rwlock_destroy(&g_rwlock);
    pthread_rwlock_init(&g_rwlock, nullptr);

    BenchResult seq = run_seqlock_bench(cfg);
    BenchResult rw  = run_rwlock_bench(cfg);

    printf("  seqlock invalid reads: %zu\n",  (size_t)seq.invalid_reads);
    printf("  rwlock  invalid reads: %zu\n\n", (size_t)rw.invalid_reads);

    seq.read_hist.print( "seqlock  read ");
    rw.read_hist.print(  "rwlock   read ");
    printf("\n");
    seq.write_hist.print("seqlock  write");
    rw.write_hist.print( "rwlock   write");

    if (seq.read_hist.count > 0 && rw.read_hist.count > 0) {
        double p50_speedup  = (double)rw.read_hist.percentile(50)  /
                              (double)seq.read_hist.percentile(50);
        double p99_speedup  = (double)rw.read_hist.percentile(99)  /
                              (double)seq.read_hist.percentile(99);
        printf("\n  read speedup  p50: %.1fx  p99: %.1fx\n",
               p50_speedup, p99_speedup);
    }

    assert(seq.invalid_reads == 0 && "seqlock correctness failure");
}

int main() {
    printf("=== Seqlock vs pthread_rwlock Benchmark ===\n");
    printf("TopOfBook size: %zu bytes\n", sizeof(TopOfBook));

    // warmup
    {
        for (size_t i = 0; i < 10000; i++) {
            TopOfBook t = make_tob(10000, i);
            g_seqlock.write(t);
            g_seqlock.read();
            pthread_rwlock_wrlock(&g_rwlock);
            g_rwlock_tob = t;
            pthread_rwlock_unlock(&g_rwlock);
            pthread_rwlock_rdlock(&g_rwlock);
            TopOfBook tmp = g_rwlock_tob;
            pthread_rwlock_unlock(&g_rwlock);
            (void)tmp;
        }
    }

    run_scenario("Read-heavy (99% reads)", {2, 100, 100'000});
    run_scenario("Balanced (50/50)",       {2,   1, 100'000});
    run_scenario("Read-heavy 4 readers",   {4, 100, 100'000});

    printf("\nAll benchmarks complete.\n");
    return 0;
}