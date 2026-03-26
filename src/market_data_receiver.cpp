#include "shared_memory.h"
#include "ring_buffer.h"
#include "signal_handler.h"
#include "virtual_memory_manager.h"
#include "slab_allocator.h"
#include "order_book.h"
#include "seqlock.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <time.h>

static constexpr size_t      RING_CAPACITY = 1024;
static constexpr size_t      NUM_MESSAGES  = 1'000'000;
static constexpr size_t      MEM_SIZE      = 4ULL * 1024 * 1024 * 1024;
static constexpr size_t      SLAB_CAP      = 64ULL * 1024 * 1024;
static constexpr const char* SHM_RING      = "/trading_ringbuf";
static constexpr const char* SHM_TOB       = "/trading_tob";

// globals for signal handler cleanup
static SharedMemory* g_shm_ring = nullptr;
static SharedMemory* g_shm_tob  = nullptr;

static void on_signal(int signo) {
    fprintf(stderr, "\nmarket_data_receiver: caught signal %d, cleaning up...\n", signo);
    if (g_shm_ring) g_shm_ring->cleanup();
    if (g_shm_tob)  g_shm_tob->cleanup();
    _exit(0);
}

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
}

int main() {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    // ---- memory management setup ----
    VirtualAddressSpace vas(MEM_SIZE);
    RegionManager rm(vas);
    install_signal_handler(&rm);

    SlabAllocator slab(rm, "ORDER_POOL", SLAB_CAP, sizeof(Order));
    if (!slab.is_valid()) {
        fprintf(stderr, "market_data_receiver: slab init failed\n");
        return 1;
    }

    // ---- shared memory setup ----
    printf("market_data_receiver: creating shared memory regions\n");

    SharedMemory shm_ring = SharedMemory::create(SHM_RING,
                                sizeof(RingBuffer<RING_CAPACITY>));
    SharedMemory shm_tob  = SharedMemory::create(SHM_TOB,
                                sizeof(Seqlock<TopOfBook>));
    g_shm_ring = &shm_ring;
    g_shm_tob  = &shm_tob;

    shm_ring.debug_dump();
    shm_tob.debug_dump();

    // placement construct ring buffer and seqlock into shared memory
    RingBuffer<RING_CAPACITY>* rb = 
        new (shm_ring.get_ptr()) RingBuffer<RING_CAPACITY>();

    Seqlock<TopOfBook>* tob_sl = 
        new (shm_tob.get_ptr()) Seqlock<TopOfBook>();

    OrderBook order_book(slab, tob_sl);

    printf("market_data_receiver: starting — sending %zu messages\n\n", NUM_MESSAGES);

    // ---- price simulation state ----
    uint64_t mid_price   = 10000;
    uint64_t total_spins = 0;
    uint64_t t_start     = now_ns();

    for (size_t i = 0; i < NUM_MESSAGES; i++) {
        // random walk — move mid price up or down by 1
        int move = (int)(i % 3) - 1;
        if (mid_price + move > 0) mid_price += move;

        // alternate bid/ask/both updates
        uint8_t msg_type = (i % 3 == 0) ? 1 :
                           (i % 3 == 1) ? 2 : 3;

        Message m{};
        m.sequence  = i;
        m.timestamp = now_ns();
        m.price     = (msg_type == 2) ? mid_price + 5 : mid_price - 5;
        m.quantity  = 100 + (i % 50);
        m.type      = msg_type;
        strncpy(m.symbol, "AAPL", sizeof(m.symbol) - 1);

        // push to ring buffer — spin if full
        uint64_t spins = 0;
        while (!rb->try_push(m)) spins++;
        total_spins += spins;

        // update order book
        order_book.update(m);

        if (i > 0 && i % 100'000 == 0) {
            order_book.print();
            printf("  sent: %zu / %zu\n\n", i, NUM_MESSAGES);
        }
    }

    // wait for consumer to drain
    printf("market_data_receiver: waiting for consumer to drain ring buffer...\n");
    while (rb->size() > 0) usleep(1000);
    usleep(50000);

    uint64_t t_end    = now_ns();
    double elapsed_s  = (t_end - t_start) / 1e9;
    double throughput = NUM_MESSAGES / elapsed_s;

    printf("\nmarket_data_receiver: done\n");
    printf("  messages sent:   %zu\n",   NUM_MESSAGES);
    printf("  elapsed:         %.3f s\n", elapsed_s);
    printf("  throughput:      %.0f msg/s\n", throughput);
    printf("  total spins:     %zu\n",   total_spins);

    slab.debug_dump();

    g_shm_ring = nullptr;
    g_shm_tob  = nullptr;
    return 0;
}