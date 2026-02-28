#include "virtual_memory_manager.h"
#include "signal_handler.h"
#include <unistd.h>
#include <cstdio>
#include <string.h>
#include <string>

constexpr size_t MEM_SIZE = 4000000000;
static RegionManager* g_region_manager = nullptr;

void print_memory_usage(const char* label) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return;

    char line[128];
    printf("--- %s ---\n", label);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmSize:", 7) == 0 ||
            strncmp(line, "VmRSS:",  6) == 0) {
            printf("%s", line);
        }
    }
    fclose(f);
}

void touch_pages(void* base, size_t offset, size_t size) {
    char* p = static_cast<char*>(base) + offset;
    for (size_t i = 0; i < size; i += 4096) {
        p[i] = 42;
    }
}

int main(int argc, char* argv[]) {
    // ----------------------------------------
    // Layer 1: VirtualAddressSpace demo
    // ----------------------------------------
    printf("========== LAYER 1: Virtual Address Space ==========\n");
    print_memory_usage("before reservation");

    VirtualAddressSpace vas(MEM_SIZE);
    print_memory_usage("after reservation");

    vas.commit(0, 100 * 1024 * 1024);
    touch_pages(vas.get_base_addr(), 0, 100 * 1024 * 1024);
    print_memory_usage("after committing + touching 100MB");

    vas.uncommit(0, 100 * 1024 * 1024);
    print_memory_usage("after uncommit");

    // ----------------------------------------
    // Layer 2: RegionManager demo
    // ----------------------------------------
    printf("\n========== LAYER 2: Region Manager ==========\n");

    RegionManager rm(vas);
    install_signal_handler(&rm);

    auto order_id = rm.allocate_region("ORDER_POOL", 512 * 1024 * 1024, AllocatorType::SLAB);
    auto market_id = rm.allocate_region("MARKET_DATA", 128 * 1024 * 1024, AllocatorType::BUMP);
    auto meta_id = rm.allocate_region("METADATA", 16 * 1024 * 1024, AllocatorType::BUMP);

    if (!order_id || !market_id || !meta_id) {
        fprintf(stderr, "Failed to allocate regions\n");
        return 1;
    }

    printf("\nInitial state after creating 3 regions:\n");
    rm.debug_dump();

    // expand ORDER_POOL to simulate slab needing more memory
    rm.expand_region(order_id.value(), 4 * 1024 * 1024);
    printf("\nAfter expanding ORDER_POOL by 4MB:\n");
    rm.debug_dump();

    // touch the committed memory in ORDER_POOL to verify it's backed
    const Region& order_region = rm.regions[order_id.value()];
    char* base = static_cast<char*>(vas.get_base_addr());
    char* order_base = base + order_region.start_offset;
    for (size_t i = 0; i < order_region.bytes_committed; i += 4096) {
        order_base[i] = 1;
    }
    print_memory_usage("after touching ORDER_POOL committed pages");

    // test lookup_by_name
    printf("\nlookup_by_name(\"MARKET_DATA\") = %zu\n", rm.lookup_by_name("MARKET_DATA").value());
    auto result = rm.lookup_by_name("NONEXISTENT");
    printf("lookup_by_name(NONEXISTENT) = %s (expect NULLOPT)\n", 
       result.has_value() ? std::to_string(result.value()).c_str() : "NULLOPT");

    // test lookup_by_addr — point into the middle of ORDER_POOL
    void* test_addr = order_base + 1024;
    printf("lookup_by_addr(order_base + 1024) = %zu (expect %zu)\n",
           rm.lookup_by_addr(test_addr).value(), order_id.value());

    // ----------------------------------------
    // Layer 3: Guard page violation demo
    // ----------------------------------------
    printf("\n========== LAYER 3: Guard Page Violation ==========\n");
    printf("About to write past the end of ORDER_POOL...\n");
    char* overflow_addr = order_base + order_region.capacity;
    *overflow_addr = 0xFF; 

    printf("ERROR: should have been caught by guard page handler\n");
    return 1;
}