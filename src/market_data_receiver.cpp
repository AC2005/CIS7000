#include "shared_memory.h"
#include "ring_buffer.h"
#include "signal_handler.h"
#include "virtual_memory_manager.h"
#include "slab_allocator.h"
#include "order_book.h"
#include "seqlock.h"
#include "heartbeat.h"
#include "latency.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <time.h>

static constexpr size_t      RING_CAPACITY = 1024;
static constexpr size_t      MEM_SIZE      = 4ULL * 1024 * 1024 * 1024;
static constexpr size_t      SLAB_CAP      = 64ULL * 1024 * 1024;
static constexpr const char* SHM_RING      = "/trading_ringbuf";
static constexpr const char* SHM_TOB       = "/trading_tob";
static constexpr const char* SHM_HEARTBEAT = "/trading_heartbeat";
static constexpr const char* SHM_LATENCY   = "/trading_latency";

static volatile bool  g_running  = true;
static SharedMemory*  g_shm_ring = nullptr;
static SharedMemory*  g_shm_tob  = nullptr;
static SharedMemory*  g_shm_hb   = nullptr;
static SharedMemory*  g_shm_lat  = nullptr;

static void on_signal(int signo) {
    fprintf(stderr, "\nmarket_data_receiver: caught signal %d\n", signo);
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

    // ---- memory management ----
    VirtualAddressSpace vas(MEM_SIZE);
    RegionManager rm(vas);
    install_signal_handler(&rm);

    SlabAllocator slab(rm, "ORDER_POOL", SLAB_CAP, sizeof(Order));
    if (!slab.is_valid()) {
        fprintf(stderr, "market_data_receiver: slab init failed\n");
        return 1;
    }

    // ---- shared memory ----
    printf("market_data_receiver: creating shared memory regions\n");

    SharedMemory shm_ring = SharedMemory::create(SHM_RING,
                                sizeof(RingBuffer<RING_CAPACITY>));
    SharedMemory shm_tob  = SharedMemory::create(SHM_TOB,
                                sizeof(Seqlock<TopOfBook>));
    SharedMemory shm_lat  = SharedMemory::create(SHM_LATENCY,
                                sizeof(LatencyRegion));
    SharedMemory shm_hb   = SharedMemory::attach(SHM_HEARTBEAT,
                                sizeof(HeartbeatRegion));

    g_shm_ring = &shm_ring;
    g_shm_tob  = &shm_tob;
    g_shm_lat  = &shm_lat;
    g_shm_hb   = &shm_hb;

    // placement construct into shared memory
    RingBuffer<RING_CAPACITY>* rb =
        new (shm_ring.get_ptr()) RingBuffer<RING_CAPACITY>();

    Seqlock<TopOfBook>* tob_sl =
        new (shm_tob.get_ptr()) Seqlock<TopOfBook>();

    LatencyRegion* lat =
        new (shm_lat.get_ptr()) LatencyRegion();
    lat->init();

    OrderBook order_book(slab, tob_sl);

    // ---- heartbeat ----
    HeartbeatRegion* hb_region =
        static_cast<HeartbeatRegion*>(shm_hb.get_ptr());
    Heartbeat& my_hb = hb_region->beats[(size_t)ProcessRole::MARKET_DATA_RECEIVER];
    printf("market_data_receiver: starting as generation %zu\n\n",
           (size_t)my_hb.generation.load(std::memory_order_acquire));

    // ---- main loop ----
    uint64_t mid_price  = 10000;
    uint64_t t_start    = now_ns();

    for (size_t i = 0; g_running; i++) {
        int move = (int)(i % 3) - 1;
        if ((int64_t)mid_price + move > 10) mid_price += move;

        uint8_t msg_type = (i % 3 == 0) ? 1 :
                           (i % 3 == 1) ? 2 : 3;

        Message m{};
        m.sequence  = i;
        m.timestamp = now_ns();   // generation timestamp for pipeline latency
        m.price     = (msg_type == 2) ? mid_price + 5 : mid_price - 5;
        m.quantity  = 100 + (i % 50);
        m.type      = msg_type;
        strncpy(m.symbol, "AAPL", sizeof(m.symbol) - 1);

        // measure push latency
        uint64_t push_t0 = now_ns();
        while (!rb->try_push(m)) {}
        uint64_t push_t1 = now_ns();
        lat->ring_push.record(push_t1 - push_t0);
        lat->total_pushed.fetch_add(1, std::memory_order_relaxed);

        // measure order book update latency
        uint64_t ob_t0 = now_ns();
        order_book.update(m);
        uint64_t ob_t1 = now_ns();
        lat->order_book_update.record(ob_t1 - ob_t0);

        // heartbeat
        if (i % 1000 == 0) my_hb.beat();

        // status every 1M messages
        if (i > 0 && i % 1'000'000 == 0) {
            double elapsed = (now_ns() - t_start) / 1e9;
            printf("market_data_receiver: %zu msgs  %.0f msg/s\n",
                   i, (double)i / elapsed);
            order_book.print();
        }
    }

    // ---- shutdown ----
    printf("market_data_receiver: shutting down\n");
    my_hb.shutdown();

    size_t attempts = 0;
    while (rb->size() > 0 && attempts++ < 1000) usleep(1000);

    double elapsed_s = (now_ns() - t_start) / 1e9;
    size_t pushed    = (size_t)lat->total_pushed.load();
    printf("market_data_receiver: done  pushed=%zu  throughput=%.0f msg/s\n",
           pushed, pushed / elapsed_s);

    slab.debug_dump();
    return 0;
}