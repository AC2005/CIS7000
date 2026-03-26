#include "order_book.h"
#include <cstring>
#include <cstdio>
#include <time.h>

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1'000'000'000ULL + ts.tv_nsec;
}

OrderBook::OrderBook(SlabAllocator& slab, Seqlock<TopOfBook>* tob)
    : slab(slab)
    , tob(tob)
    , order_id(0)
    , best_bid(nullptr)
    , best_ask(nullptr)
{}

void OrderBook::update(const Message& msg) {
    void* slot = slab.allocate();
    if (!slot) {
        fprintf(stderr, "OrderBook::update: slab allocation failed\n");
        return;
    }

    Order* order     = static_cast<Order*>(slot);
    order->id        = order_id++;
    order->price     = msg.price;
    order->quantity  = msg.quantity;
    order->timestamp = now_ns();
    strncpy(order->symbol, msg.symbol, sizeof(order->symbol) - 1);
    order->symbol[sizeof(order->symbol) - 1] = '\0';

    bool tob_changed = false;

    if (msg.type == 1) {
        // bid update
        order->side = 0;
        if (best_bid == nullptr || order->price > best_bid->price ||
           (order->price == best_bid->price &&
            order->quantity != best_bid->quantity)) {
            if (best_bid != nullptr) slab.deallocate(best_bid);
            best_bid    = order;
            tob_changed = true;
        } else {
            slab.deallocate(order);
        }

    } else if (msg.type == 2) {
        // ask update
        order->side = 1;
        if (best_ask == nullptr || order->price < best_ask->price ||
           (order->price == best_ask->price &&
            order->quantity != best_ask->quantity)) {
            if (best_ask != nullptr) slab.deallocate(best_ask);
            best_ask    = order;
            tob_changed = true;
        } else {
            slab.deallocate(order);
        }

    } else if (msg.type == 3) {
        // both sides
        order->side = 0;
        if (best_bid != nullptr) slab.deallocate(best_bid);
        best_bid = order;

        void* ask_slot = slab.allocate();
        if (!ask_slot) {
            fprintf(stderr, "OrderBook::update: ask slab allocation failed\n");
            return;
        }
        Order* ask      = static_cast<Order*>(ask_slot);
        ask->id         = order_id++;
        ask->price      = msg.price + 10;
        ask->quantity   = msg.quantity;
        ask->timestamp  = now_ns();
        ask->side       = 1;
        strncpy(ask->symbol, msg.symbol, sizeof(ask->symbol) - 1);
        ask->symbol[sizeof(ask->symbol) - 1] = '\0';

        if (best_ask != nullptr) slab.deallocate(best_ask);
        best_ask    = ask;
        tob_changed = true;
    }

    if (tob_changed && best_bid != nullptr && best_ask != nullptr) {
        write_tob();
    }
}

void OrderBook::write_tob() {
    TopOfBook snapshot;
    snapshot.bid_price    = best_bid->price;
    snapshot.bid_quantity = best_bid->quantity;
    snapshot.ask_price    = best_ask->price;
    snapshot.ask_quantity = best_ask->quantity;
    snapshot.last_update  = now_ns();
    strncpy(snapshot.symbol, best_bid->symbol, sizeof(snapshot.symbol) - 1);
    snapshot.symbol[sizeof(snapshot.symbol) - 1] = '\0';
    memset(snapshot.padding, 0, sizeof(snapshot.padding));

    tob->write(snapshot);
}

TopOfBook OrderBook::read() const {
    return tob->read();
}

void OrderBook::print() const {
    TopOfBook snapshot = read();
    printf("=== OrderBook ===\n");
    snapshot.print();
    printf("  valid: %s\n", snapshot.is_valid() ? "yes" : "no");
}