#include "shared_memory.h"
#include "ring_buffer.h"
#include "order_book.h"
#include "seqlock.h"
#include "heartbeat.h"
#include "latency.h"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <time.h>
#include <atomic>

static constexpr size_t      RING_CAPACITY    = 1024;
static constexpr const char* SHM_RING         = "/trading_ringbuf";
static constexpr const char* SHM_TOB          = "/trading_tob";
static constexpr const char* SHM_HEARTBEAT    = "/trading_heartbeat";
static constexpr const char* SHM_LATENCY      = "/trading_latency";
static constexpr uint64_t    SPREAD_THRESHOLD = 8;

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

    printf("strategy_engine: attaching to shared memory\n");

    SharedMemory shm_ring = SharedMemory::attach(SHM_RING,
                                sizeof(RingBuffer<RING_CAPACITY>));
    SharedMemory shm_tob  = SharedMemory::attach(SHM_TOB,
                                sizeof(Seqlock<TopOfBook>));
    SharedMemory shm_lat  = SharedMemory::attach(SHM_LATENCY,
                                sizeof(LatencyRegion));
    SharedMemory shm_hb   = SharedMemory::attach(SHM_HEARTBEAT,
                                sizeof(HeartbeatRegion));

    RingBuffer<RING_CAPACITY>* rb =
        static_cast<RingBuffer<RING_CAPACITY>*>(shm_ring.get_ptr());

    Seqlock<TopOfBook>* tob_sl =
        static_cast<Seqlock<TopOfBook>*>(shm_tob.get_ptr());

    LatencyRegion* lat =
        static_cast<LatencyRegion*>(shm_lat.get_ptr());

    HeartbeatRegion* hb_region =
        static_cast<HeartbeatRegion*>(shm_hb.get_ptr());

    Heartbeat& my_hb   = hb_region->beats[(size_t)ProcessRole::STRATEGY_ENGINE];
    Heartbeat& peer_hb = hb_region->beats[(size_t)ProcessRole::MARKET_DATA_RECEIVER];
    uint64_t peer_gen  = peer_hb.generation.load(std::memory_order_acquire);

    printf("strategy_engine: running\n\n");

    uint64_t expected_seq = 0;
    uint64_t t_start      = now_ns();

    while (g_running) {

        // ---- consume from ring buffer ----
        Message m{};
        if (rb->try_pop(m)) {
            uint64_t pop_time = now_ns();

            // ring IPC latency — push timestamp to pop time
            lat->ring_pop_latency.record(pop_time - m.timestamp);
            lat->total_popped.fetch_add(1, std::memory_order_relaxed);

            // sequence check
            if (m.sequence != expected_seq) {
                lat->seq_errors.fetch_add(1, std::memory_order_relaxed);
                expected_seq = m.sequence + 1;
            } else {
                expected_seq++;
            }

            // ---- read top of book ----
            uint64_t tob_t0 = now_ns();
            TopOfBook snapshot = tob_sl->read();
            uint64_t tob_t1 = now_ns();
            lat->tob_read.record(tob_t1 - tob_t0);

            // full pipeline latency — message generation to strategy decision
            lat->pipeline.record(tob_t1 - m.timestamp);

            // validate
            if (snapshot.bid_price > 0 && !snapshot.is_valid()) {
                lat->invalid_tob_reads.fetch_add(1, std::memory_order_relaxed);
                fprintf(stderr, "strategy_engine: INVALID TOB bid=%zu ask=%zu\n",
                        (size_t)snapshot.bid_price,
                        (size_t)snapshot.ask_price);
            }

            // strategy decision
            if (snapshot.bid_price > 0 && snapshot.ask_price > 0) {
                uint64_t spread = snapshot.ask_price - snapshot.bid_price;
                if (spread <= SPREAD_THRESHOLD) {
                    uint64_t signals = lat->trade_signals.fetch_add(
                        1, std::memory_order_relaxed);
                    if (signals < 3) {
                        printf("TRADE SIGNAL #%zu: ", (size_t)(signals + 1));
                        snapshot.print();
                    }
                }
            }
        }

        // ---- heartbeat + peer restart detection ----
        if (expected_seq % 1000 == 0) {
            my_hb.beat();

            uint64_t cur_gen = peer_hb.generation.load(std::memory_order_acquire);
            if (cur_gen != peer_gen) {
                printf("strategy_engine: peer restart detected "
                       "(gen %zu → %zu) resyncing\n",
                       (size_t)peer_gen, (size_t)cur_gen);
                peer_gen     = cur_gen;
                expected_seq = 0;
            }
        }

        // status every 1M messages
        if (expected_seq > 0 && expected_seq % 1'000'000 == 0) {
            double elapsed = (now_ns() - t_start) / 1e9;
            printf("strategy_engine: %zu msgs  %.0f msg/s  signals=%zu\n",
                   (size_t)expected_seq,
                   (double)expected_seq / elapsed,
                   (size_t)lat->trade_signals.load());
        }
    }

    // ---- shutdown ----
    printf("strategy_engine: shutting down\n");
    my_hb.shutdown();

    double elapsed_s = (now_ns() - t_start) / 1e9;
    size_t popped    = (size_t)lat->total_popped.load();
    printf("strategy_engine: done  popped=%zu  throughput=%.0f msg/s\n",
           popped, popped / elapsed_s);

    // print the full latency report — consumer has all the data
    lat->print_report();

    return 0;
}