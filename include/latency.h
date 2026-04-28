#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <climits>
#include <algorithm>

struct Histogram {
    static constexpr size_t NUM_BUCKETS = 64;

    char     label[32];
    uint64_t buckets[NUM_BUCKETS];
    uint64_t count;
    uint64_t total;
    uint64_t min_val;
    uint64_t max_val;

    void init(const char* name) {
        strncpy(label, name, sizeof(label) - 1);
        label[sizeof(label) - 1] = '\0';
        memset(buckets, 0, sizeof(buckets));
        count   = 0;
        total   = 0;
        min_val = UINT64_MAX;
        max_val = 0;
    }

    void record(uint64_t ns) {
        if (ns == 0) return;
        count++;
        total  += ns;
        min_val = (ns < min_val) ? ns : min_val;
        max_val = (ns > max_val) ? ns : max_val;
        int b = (ns > 0) ? (63 - __builtin_clzll(ns)) : 0;
        buckets[b < (int)NUM_BUCKETS ? b : (int)NUM_BUCKETS - 1]++;
    }

    uint64_t percentile(double p) const {
        if (count == 0) return 0;
        uint64_t target = (uint64_t)(count * p / 100.0);
        uint64_t cum    = 0;
        for (size_t i = 0; i < NUM_BUCKETS; i++) {
            cum += buckets[i];
            if (cum >= target) return 1ULL << i;
        }
        return max_val;
    }

    void print() const {
        if (count == 0) {
            printf("  %-30s  no samples\n", label);
            return;
        }
        printf("  %-30s  "
               "count: %8zu  "
               "avg: %7.1f ns  "
               "min: %5zu ns  "
               "p50: %5zu ns  "
               "p99: %6zu ns  "
               "p99.9: %7zu ns  "
               "max: %8zu ns\n",
               label,
               (size_t)count,
               (double)total / count,
               (size_t)min_val,
               (size_t)percentile(50),
               (size_t)percentile(99),
               (size_t)percentile(99.9),
               (size_t)max_val);
    }
};


struct LatencyRegion {
    // stamped by receiver
    Histogram ring_push;        // time to complete try_push (spin included)
    Histogram order_book_update;// time to complete order_book.update()

    // stamped by consumer
    Histogram ring_pop_latency; // now_ns() - msg.timestamp (push to pop)
    Histogram tob_read;         // time to complete tob->read()
    Histogram pipeline;         // now_ns() - msg.timestamp after tob read
                                // (full pipeline: generation → strategy decision)

    // counters
    std::atomic<uint64_t> total_pushed;
    std::atomic<uint64_t> total_popped;
    std::atomic<uint64_t> seq_errors;
    std::atomic<uint64_t> invalid_tob_reads;
    std::atomic<uint64_t> trade_signals;

    void init() {
        ring_push.init("ring_push");
        order_book_update.init("order_book_update");
        ring_pop_latency.init("ring_pop_latency");
        tob_read.init("tob_read");
        pipeline.init("full_pipeline");
        total_pushed.store(0);
        total_popped.store(0);
        seq_errors.store(0);
        invalid_tob_reads.store(0);
        trade_signals.store(0);
    }

    void print_report() const {
        printf("\n========================================\n");
        printf("  End-to-End Latency Report\n");
        printf("========================================\n");
        printf("  messages pushed:      %zu\n",
               (size_t)total_pushed.load());
        printf("  messages popped:      %zu\n",
               (size_t)total_popped.load());
        printf("  sequence errors:      %zu\n",
               (size_t)seq_errors.load());
        printf("  invalid tob reads:    %zu\n",
               (size_t)invalid_tob_reads.load());
        printf("  trade signals:        %zu\n\n",
               (size_t)trade_signals.load());

        printf("  --- Receiver side ---\n");
        ring_push.print();
        order_book_update.print();

        printf("\n  --- Consumer side ---\n");
        ring_pop_latency.print();
        tob_read.print();
        pipeline.print();

        printf("\n  --- Key targets ---\n");
        printf("  ring IPC p50:         %zu ns  (target: <1000 ns)\n",
               (size_t)ring_pop_latency.percentile(50));
        printf("  ring IPC p99:         %zu ns  (target: <10000 ns)\n",
               (size_t)ring_pop_latency.percentile(99));
        printf("  full pipeline p50:    %zu ns  (target: <10000 ns)\n",
               (size_t)pipeline.percentile(50));
        printf("  full pipeline p99:    %zu ns  (target: <10000 ns)\n",
               (size_t)pipeline.percentile(99));

        bool ipc_ok      = ring_pop_latency.percentile(50) < 1000;
        bool pipeline_ok = pipeline.percentile(50) < 10000;
        printf("\n  IPC latency target:      %s\n",
               ipc_ok      ? "PASS" : "FAIL");
        printf("  Pipeline latency target: %s\n",
               pipeline_ok ? "PASS" : "FAIL");
        printf("========================================\n\n");
    }
};