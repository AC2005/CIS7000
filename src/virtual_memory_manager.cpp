#include "virtual_memory_manager.h"
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <string.h>


static size_t get_page_size() {
    static const size_t page_size = static_cast<size_t>(sysconf(_SC_PAGE_SIZE));
    return page_size;
}

/*
mprotect and madvise expected page aligned values, so this pair of helper functions
aligns the input value to the nearest multiple of page_size
*/
static inline size_t align_down(size_t value, size_t page_size) {
    return value & ~(page_size - 1);
}

static inline size_t align_up(size_t value, size_t page_size) {
    return (value + page_size - 1) & ~(page_size - 1);
}

/* reserves portion of memory on startup*/
VirtualAddressSpace::VirtualAddressSpace(size_t mem_size) 
    : base_addr(mmap(NULL, mem_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)), mem_size(mem_size)
{
    if (base_addr == MAP_FAILED) {
        perror("VirtualAddressSpace: mmap failed");
        std::abort();
    }
}

VirtualAddressSpace::~VirtualAddressSpace() {
   if (munmap(base_addr, mem_size) == -1) {
        perror("VirtualAddressSpace: munmap failed");
   }
}

/* 
Allows reads and writes to the region, and upon access will page fault and back a physical
page to the region 
*/
void VirtualAddressSpace::commit(size_t offset, size_t size) {
    // need to make sure it's page aligned
    const size_t page_size = get_page_size();
    uintptr_t base = reinterpret_cast<uintptr_t>(base_addr);
    uintptr_t start = base + offset;
    uintptr_t end = start + size;

    uintptr_t aligned_start = align_down(start, page_size);
    uintptr_t aligned_size = align_up(end - aligned_start, page_size);

    if (mprotect(reinterpret_cast<void*>(aligned_start), aligned_size, PROT_READ | PROT_WRITE) == -1) {
        perror("VirtualAddressSpace: commit mprotect failed");
        std::abort();
    }
}

/* 
"erases" link to physical page and upon access will page fault
*/
void VirtualAddressSpace::uncommit(size_t offset, size_t size) {
    const size_t page_size = get_page_size();
    uintptr_t base = reinterpret_cast<uintptr_t>(base_addr);
    uintptr_t start = base + offset;
    uintptr_t end = start + size;

    uintptr_t aligned_start = align_down(start, page_size);
    uintptr_t aligned_size = align_up(end - aligned_start, page_size);
    
    // PROT_NONE means CPU will page fault on access
    if (mprotect(reinterpret_cast<void*>(aligned_start), aligned_size, PROT_NONE) == -1) {
        perror("VirtualAddressSpace: uncommit mprotect failed");
        std::abort();
    }
    // allows kernel to reclaim physical pages
    if (madvise(reinterpret_cast<void*>(aligned_start), aligned_size, MADV_FREE) == -1) {
        perror("VirtualAddressSpace: uncommit madvise failed");
    }
}

std::optional<size_t> RegionManager::allocate_region(const char* name, size_t capacity, AllocatorType allocator) {
    size_t capacity_aligned = align_up(capacity, get_page_size());
    if (end_offset + capacity_aligned + 2 * get_page_size() > vas.get_mem_size()) {
        fprintf(stderr, "Cannot allocate another region");
        return std::nullopt;
    }
    size_t mem_start_offset = end_offset + get_page_size(); 
    size_t mem_end_offset = mem_start_offset + capacity_aligned + get_page_size();
    // commit a little bit
    size_t to_commit = std::min(2 * get_page_size(), capacity_aligned);
    vas.commit(mem_start_offset, to_commit);

    // bookkeeping
    Region curr_region;
    strncpy(curr_region.name, name, sizeof(curr_region.name) - 1);
    curr_region.name[31] = '\0'; 
    curr_region.start_offset = mem_start_offset;
    curr_region.capacity = capacity_aligned;
    curr_region.bytes_committed = to_commit;
    curr_region.allocator = allocator;

    regions[region_count] = curr_region;
    region_count += 1;
    end_offset = mem_end_offset;

    return region_count - 1;
}

bool RegionManager::expand_region(size_t region_id, size_t num_bytes) {
    if (region_id >= region_count) {
        fprintf(stderr, "Cannot find region");
        return false;
    } 

    Region& r = regions[region_id];
    size_t aligned = align_up(num_bytes, get_page_size());
    if (r.bytes_committed + aligned > r.capacity) {
        fprintf(stderr, "Expansion would cause overflow");
        return false;
    }

    vas.commit(r.start_offset + r.bytes_committed, aligned);
    r.bytes_committed += aligned;
    return true;
}

std::optional<size_t> RegionManager::lookup_by_addr(void* addr) {
    uintptr_t a = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(vas.get_base_addr());
    
    for (size_t i = 0; i < region_count; i++) {
        size_t start = regions[i].start_offset;
        size_t end = start + regions[i].capacity;
        if (a >= start && a < end) {
            return i;
        }
    }
    return std::nullopt;
}


std::tuple<FaultType, std::optional<size_t>> RegionManager::is_guard_page(void* addr) {
    uintptr_t a = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(vas.get_base_addr());
    
    for (size_t i = 0; i < region_count; i++) {
        size_t start = regions[i].start_offset;
        size_t end = start + regions[i].capacity;
        if (start >= get_page_size() && a >= start - get_page_size() && a < start) {
            return std::tuple<FaultType, std::optional<size_t>>(FaultType::UNDERFLOW, i);
        } else if (a >= end && a < end + get_page_size()) {
            return std::tuple<FaultType, std::optional<size_t>>(FaultType::OVERFLOW, i);
        }
    }
    return std::tuple<FaultType, std::optional<size_t>>(FaultType::NO_FAULT, std::nullopt);
}


std::optional<size_t> RegionManager::lookup_by_name(const char* name) {
    for (size_t i = 0; i < region_count; i++) {
        if (strncmp(regions[i].name, name, sizeof(regions[i].name)) == 0) {
            return i;
        }
    }

    return std::nullopt;
}

void RegionManager::debug_dump() const {
    uintptr_t base = reinterpret_cast<uintptr_t>(vas.get_base_addr());

    printf("=== RegionManager Debug Dump ===\n");
    printf("VAS base:      %p\n", vas.get_base_addr());
    printf("VAS size:      %zu MB\n", vas.get_mem_size() / (1024 * 1024));
    printf("End offset:    %zu MB\n", end_offset / (1024 * 1024));
    printf("Region count:  %zu / %zu\n", region_count, MAX_REGIONS);
    printf("--------------------------------\n");

    for (size_t i = 0; i < region_count; i++) {
        const Region& r = regions[i];
        uintptr_t abs_start = base + r.start_offset;
        uintptr_t abs_end   = abs_start + r.capacity;
        uintptr_t guard_lo  = abs_start - get_page_size();
        uintptr_t guard_hi  = abs_end;

        const char* type_str = (r.allocator == AllocatorType::SLAB) ? "SLAB" : "BUMP";

        printf("[%zu] %-24s  type: %-4s  base: 0x%lx  end: 0x%lx  "
               "capacity: %6zu KB  committed: %6zu KB  "
               "guard_lo: 0x%lx  guard_hi: 0x%lx\n",
               i,
               r.name,
               type_str,
               abs_start,
               abs_end,
               r.capacity / 1024,
               r.bytes_committed / 1024,
               guard_lo,
               guard_hi);
    }
    printf("================================\n");
}
