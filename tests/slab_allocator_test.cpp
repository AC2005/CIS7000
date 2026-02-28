#include "virtual_memory_manager.h"
#include "slab_allocator.h"
#include "signal_handler.h"
#include <cstdio>
#include <cassert>
#include <vector>
#include <string.h>

constexpr size_t MEM_SIZE = 4ULL * 1024 * 1024 * 1024;
constexpr size_t SLAB_CAP = 64ULL * 1024 * 1024;
constexpr size_t SLOT_SIZE = 64;

struct Order {
    uint64_t id;
    uint64_t price;
    uint64_t quantity;
    uint64_t timestamp;
    char     symbol[16];
    uint64_t flags;
    uint64_t reserved[1];
};
static_assert(sizeof(Order) == 64, "Order must be exactly 64 bytes");

void test_basic_alloc_free(SlabAllocator& slab) {
    printf("--- test_basic_alloc_free ---\n");

    void* p = slab.allocate();
    assert(p != nullptr && "allocation returned null");

    size_t before = slab.slots_available();
    slab.deallocate(p);
    size_t after = slab.slots_available();

    assert(after == before + 1 && "available slots should increase by 1 after free");
    printf("PASS\n");
}

void test_slots_available_tracking(SlabAllocator& slab) {
    printf("--- test_slots_available_tracking ---\n");

    size_t initial = slab.slots_available();
    constexpr size_t N = 100;
    std::vector<void*> ptrs(N);

    for (size_t i = 0; i < N; i++) {
        ptrs[i] = slab.allocate();
        assert(ptrs[i] != nullptr);
        assert(slab.slots_available() == initial - (i + 1));
    }

    for (size_t i = 0; i < N; i++) {
        slab.deallocate(ptrs[i]);
    }

    assert(slab.slots_available() == initial && "all slots should be available after freeing all");
    printf("PASS\n");
}

void test_write_read(SlabAllocator& slab) {
    printf("--- test_write_read ---\n");

    Order* o = static_cast<Order*>(slab.allocate());
    assert(o != nullptr);

    o->id        = 42;
    o->price     = 1000;
    o->quantity  = 5;
    o->timestamp = 9999;

    assert(o->id       == 42);
    assert(o->price    == 1000);
    assert(o->quantity == 5);
    assert(o->timestamp == 9999);

    slab.deallocate(o);
    printf("PASS\n");
}

void test_reuse(SlabAllocator& slab) {
    printf("--- test_reuse ---\n");

    // allocate and free the same slot repeatedly — should always get a valid pointer
    for (size_t i = 0; i < 1000; i++) {
        void* p = slab.allocate();
        assert(p != nullptr);
        static_cast<Order*>(p)->id = i;
        slab.deallocate(p);
    }

    printf("PASS\n");
}

void test_expansion(SlabAllocator& slab) {
    printf("--- test_expansion ---\n");

    // exhaust initial slots to force expansion
    size_t initial_total = slab.slots_total();
    std::vector<void*> ptrs(initial_total);

    for (size_t i = 0; i < initial_total; i++) {
        ptrs[i] = slab.allocate();
        assert(ptrs[i] != nullptr);
    }

    // this allocation should trigger expand()
    void* extra = slab.allocate();
    assert(extra != nullptr && "allocation after expansion should succeed");
    assert(slab.slots_total() > initial_total && "total slots should grow after expansion");

    // clean up
    for (size_t i = 0; i < initial_total; i++) {
        slab.deallocate(ptrs[i]);
    }
    slab.deallocate(extra);

    printf("PASS\n");
}

void test_invalid_free(SlabAllocator& slab) {
    printf("--- test_invalid_free (expect abort) ---\n");

    // this should abort — passing a stack address to deallocate
    char stack_buf[64];
    slab.deallocate(stack_buf);  // should never reach next line

    printf("FAIL: should have aborted\n");
}

void test_interleaved(SlabAllocator& slab) {
    printf("--- test_interleaved ---\n");

    constexpr size_t WINDOW = 500;
    constexpr size_t ITERATIONS = 10000;
    std::vector<void*> window(WINDOW, nullptr);
    size_t idx = 0;

    for (size_t i = 0; i < ITERATIONS; i++) {
        if (window[idx] != nullptr) {
            slab.deallocate(window[idx]);
        }
        window[idx] = slab.allocate();
        assert(window[idx] != nullptr);
        static_cast<Order*>(window[idx])->id = i;
        idx = (idx + 1) % WINDOW;
    }

    // verify data integrity of remaining live allocations
    // (we can't check values since they've been overwritten, just check non-null)
    for (size_t i = 0; i < WINDOW; i++) {
        if (window[i]) slab.deallocate(window[i]);
    }

    printf("PASS\n");
}

int main() {
    VirtualAddressSpace vas(MEM_SIZE);
    RegionManager rm(vas);
    install_signal_handler(&rm);

    SlabAllocator slab(rm, "ORDER_POOL", SLAB_CAP, SLOT_SIZE);
    if (!slab.is_valid()) {
        fprintf(stderr, "Failed to create slab allocator\n");
        return 1;
    }

    printf("=== Slab Allocator Functionality Tests ===\n\n");
    slab.debug_dump();
    printf("\n");

    test_basic_alloc_free(slab);
    test_slots_available_tracking(slab);
    test_write_read(slab);
    test_reuse(slab);
    test_expansion(slab);
    test_interleaved(slab);

    printf("\nAll tests passed.\n");
    slab.debug_dump();

    // run this last since it aborts the process
    test_invalid_free(slab);
}