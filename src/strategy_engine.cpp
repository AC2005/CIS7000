#include "shared_memory.h"
#include "ring_buffer.h"
#include "order_book.h"
#include "seqlock.h"
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <time.h>
#include <atomic>

static constexpr size_t      RING_CAPACITY = 1024;
static constexpr size_t      NUM_MESSAGES  = 1'000'000;
static constexpr const char* SHM_RING      = "/trading_ringbuf";
static constexpr const char* SHM_TOB       = "/trading_tob";
static constexpr uint64_t    SPREAD_THRESHOLD = 8;  // signal if spread <= this

static volatile bool g_running = true;

static void on_signal(int signo) {
    fprintf(stderr, "\nstrategy_engine: caught signal %d\n", signo);
    g_running = false;
}

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
}

int main() {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    // ---- attach to shared memory ----
    printf("strategy_engine: attaching to shared memory\n");

    SharedMemory shm_ring = SharedMemory::attach(SHM_RING,
                                sizeof(RingBuffer<RING_CAPACITY>));
    SharedMemory shm_tob  = SharedMemory::attach(SHM_TOB,
                                sizeof(Seqlock<TopOfBook>));

    shm_ring.debug_dump();
    shm_tob.debug_dump();

    RingBuffer<RING_CAPACITY>* rb =
        static_cast<RingBuffer<RING_CAPACITY>*>(shm_ring.get_ptr());

    Seqlock<TopOfBook>* tob_sl =
        static_cast<Seqlock<TopOfBook>*>(shm_tob.get_ptr());

    printf("strategy_engine: running\n\n");

    // ---- stats ----
    uint64_t msgs_received  = 0;
    uint64_t seq_errors     = 0;
    uint64_t invalid_reads  = 0;
    uint64_t trade_signals  = 0;
    uint64_t total_latency  = 0;
    uint64_t max_latency    = 0;
    uint64_t tob_reads      = 0;
    uint64_t expected_seq   = 0;

    uint64_t t_start = now_ns();

    while (msgs_received < NUM_MESSAGES && g_running) {

        // ---- consume from ring buffer ----
        Message m{};
        if (rb->try_pop(m)) {
            uint64_t recv_time = now_ns();

            if (m.sequence != expected_seq) {
                seq_errors++;
                fprintf(stderr, "strategy_engine: seq error expected=%zu got=%zu\n",
                        (size_t)expected_seq, (size_t)m.sequence);
                expected_seq = m.sequence + 1;
            } else {
                expected_seq++;
            }

            uint64_t latency = recv_time - m.timestamp;
            total_latency += latency;
            if (latency > max_latency) max_latency = latency;

            msgs_received++;
        }

        // ---- read top of book via seqlock ----
        TopOfBook snapshot = tob_sl->read();
        tob_reads++;

        if (snapshot.bid_price > 0 && snapshot.ask_price > 0) {
            if (!snapshot.is_valid()) {
                invalid_reads++;
                fprintf(stderr,
                    "strategy_engine: INVALID TOB READ bid=%zu ask=%zu\n",
                    (size_t)snapshot.bid_price,
                    (size_t)snapshot.ask_price);
            }

            // simple strategy: signal when spread is tight
            uint64_t spread = snapshot.ask_price - snapshot.bid_price;
            if (spread <= SPREAD_THRESHOLD) {
                trade_signals++;
                if (trade_signals <= 5) {
                    // print first few signals so we can see them
                    printf("TRADE SIGNAL #%zu: ", (size_t)trade_signals);
                    snapshot.print();
                }
            }
        }

        // print progress every 100k messages
        if (msgs_received > 0 && msgs_received % 100'000 == 0) {
            printf("strategy_engine: received %zu / %zu  "
                   "avg_latency: %zu ns  signals: %zu\n",
                   (size_t)msgs_received, NUM_MESSAGES,
                   (size_t)(total_latency / msgs_received),
                   (size_t)trade_signals);
        }
    }

    uint64_t t_end    = now_ns();
    double elapsed_s  = (t_end - t_start) / 1e9;
    double throughput = msgs_received / elapsed_s;

    printf("\nstrategy_engine: done\n");
    printf("  messages received:   %zu\n",   (size_t)msgs_received);
    printf("  sequence errors:     %zu\n",   (size_t)seq_errors);
    printf("  tob reads:           %zu\n",   (size_t)tob_reads);
    printf("  invalid tob reads:   %zu\n",   (size_t)invalid_reads);
    printf("  trade signals:       %zu\n",   (size_t)trade_signals);
    printf("  elapsed:             %.3f s\n", elapsed_s);
    printf("  throughput:          %.0f msg/s\n", throughput);
    printf("  avg msg latency:     %zu ns\n",
           msgs_received > 0 ? (size_t)(total_latency / msgs_received) : 0);
    printf("  max msg latency:     %zu ns\n",   (size_t)max_latency);

    if (seq_errors == 0 && invalid_reads == 0) {
        printf("\n  PASS: zero sequence errors, zero invalid TOB reads\n");
    } else {
        printf("\n  FAIL: %zu seq errors, %zu invalid TOB reads\n",
               (size_t)seq_errors, (size_t)invalid_reads);
        return 1;
    }

    return 0;
}