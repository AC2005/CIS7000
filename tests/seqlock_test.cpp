// tests/seqlock_test.cpp
#include "seqlock.h"
#include "order_book.h"
#include <thread>
#include <atomic>
#include <cstdio>
#include <cassert>
#include <cstring>

constexpr size_t NUM_WRITES  = 100'000;
constexpr size_t NUM_READERS = 2;

void test_seqlock_correctness() {
    printf("--- test_seqlock_correctness ---\n");

    Seqlock<TopOfBook> sl;
    std::atomic<bool>  done{false};
    std::atomic<uint64_t> invalid_reads{0};
    std::atomic<uint64_t> total_reads{0};

    // writer thread — simulates market data receiver
    std::thread writer([&]() {
        uint64_t mid = 10000;
        for (size_t i = 0; i < NUM_WRITES; i++) {
            // random walk
            mid += (i % 3) - 1;

            TopOfBook tob{};
            tob.bid_price    = mid - 5;
            tob.bid_quantity = 100 + (i % 50);
            tob.ask_price    = mid + 5;
            tob.ask_quantity = 100 + (i % 30);
            tob.last_update  = i;
            strncpy(tob.symbol, "AAPL", sizeof(tob.symbol) - 1);

            sl.write(tob);
        }
        done.store(true, std::memory_order_release);
    });

    // reader threads — simulate strategy engine
    auto reader_fn = [&]() {
        while (!done.load(std::memory_order_acquire)) {
            TopOfBook snapshot = sl.read();

            // only validate if initialized
            if (snapshot.bid_price > 0 && snapshot.ask_price > 0) {
                if (!snapshot.is_valid()) {
                    invalid_reads.fetch_add(1, std::memory_order_relaxed);
                    fprintf(stderr, "INVALID READ: bid=%zu ask=%zu\n",
                            (size_t)snapshot.bid_price,
                            (size_t)snapshot.ask_price);
                }
                total_reads.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread readers[NUM_READERS];
    for (size_t i = 0; i < NUM_READERS; i++) {
        readers[i] = std::thread(reader_fn);
    }

    writer.join();
    for (size_t i = 0; i < NUM_READERS; i++) readers[i].join();

    printf("  total reads:   %zu\n", (size_t)total_reads.load());
    printf("  invalid reads: %zu\n", (size_t)invalid_reads.load());
    assert(invalid_reads == 0 && "seqlock correctness failure — torn read detected");
    printf("PASS\n");
}

void test_seqlock_single_threaded() {
    printf("--- test_seqlock_single_threaded ---\n");

    Seqlock<TopOfBook> sl;

    TopOfBook tob{};
    tob.bid_price    = 9995;
    tob.bid_quantity = 100;
    tob.ask_price    = 10005;
    tob.ask_quantity = 200;
    strncpy(tob.symbol, "AAPL", sizeof(tob.symbol) - 1);

    sl.write(tob);

    TopOfBook snapshot = sl.read();
    assert(snapshot.bid_price    == 9995);
    assert(snapshot.ask_price    == 10005);
    assert(snapshot.bid_quantity == 100);
    assert(snapshot.ask_quantity == 200);
    assert(snapshot.is_valid());
    printf("PASS\n");
}

void test_seqlock_try_read() {
    printf("--- test_seqlock_try_read ---\n");

    Seqlock<TopOfBook> sl;

    TopOfBook tob{};
    tob.bid_price = 9990;
    tob.ask_price = 10010;
    sl.write(tob);

    TopOfBook out{};
    bool ok = sl.try_read(out);
    assert(ok && "try_read should succeed when no write in progress");
    assert(out.bid_price == 9990);
    assert(out.ask_price == 10010);
    printf("PASS\n");
}

int main() {
    printf("=== Seqlock Tests ===\n\n");
    test_seqlock_single_threaded();
    test_seqlock_try_read();
    test_seqlock_correctness();
    printf("\nAll seqlock tests passed.\n");
    return 0;
}