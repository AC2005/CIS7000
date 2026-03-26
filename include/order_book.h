#pragma once
#include "slab_allocator.h"
#include "ring_buffer.h"
#include "seqlock.h"
#include <cstdint>
#include <atomic>
#include <cstdio>
#include <cstring>

struct Order {
    uint64_t id;
    uint64_t price;
    uint64_t quantity;
    uint64_t timestamp;
    char     symbol[16];
    uint8_t  side;
    uint8_t  padding[15];
};
static_assert(sizeof(Order) == 64, "Order must be 64 bytes");

struct TopOfBook {
    uint64_t bid_price;
    uint64_t bid_quantity;
    uint64_t ask_price;
    uint64_t ask_quantity;
    uint64_t last_update;
    char     symbol[16];
    uint8_t  padding[8];

    bool is_valid() const {
        if (bid_price == 0 || ask_price == 0) return false;
        if (bid_price >= ask_price)            return false;
        return true;
    }

    void print() const {
        printf("  bid: %zu x %zu  |  ask: %zu x %zu  |  spread: %zu  |  symbol: %s\n",
               (size_t)bid_price,    (size_t)bid_quantity,
               (size_t)ask_price,    (size_t)ask_quantity,
               (size_t)(ask_price - bid_price),
               symbol);
    }
};
static_assert(sizeof(TopOfBook) == 64, "TopOfBook must be 64 bytes");

class OrderBook {
public:
    // tob lives in shared memory — passed in as a pointer
    OrderBook(SlabAllocator& slab, Seqlock<TopOfBook>* tob);

    void      update(const Message& msg);
    TopOfBook read()  const;
    void      print() const;

private:
    SlabAllocator&        slab;
    Seqlock<TopOfBook>*   tob;
    uint64_t              order_id;
    Order*                best_bid;
    Order*                best_ask;

    void write_tob();
};