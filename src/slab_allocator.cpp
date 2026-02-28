#include "slab_allocator.h"

#include <stdio.h>
#include <cassert>

SlabAllocator::SlabAllocator(RegionManager &rm, const char* name, size_t capacity, size_t slot_size): rm(rm), slot_size(slot_size) {
    assert(slot_size >= sizeof(void*) && "slot size must be large enough to hold a pointer");
    assert(slot_size % 8 == 0 && "slot size must be 8-byte aligned");

    std::optional<size_t> r = rm.allocate_region(name, capacity, AllocatorType::SLAB);
    if (r == std::nullopt) {
        fprintf(stderr, "SlabAllocator: region manager failed to allocate region");
        return;
    }
    region_id = r.value();
    
    // get underlying region_struct, should always work
    std::optional<Region> re = rm.get_region(region_id);
    if (re == std::nullopt) { 
        fprintf(stderr, "SlabAllocator: region manager failed to retrieve region");
        return;
    }

    Region region = re.value();

    // create free list pointers, each slot holds a pointer to the next slot;
    free_list_head = rm.get_absolute_addr(region.start_offset);

    // treat the current address as a pointer to a void* so that we can write a void* (the next free slab) to it
    void** curr = static_cast<void**>(free_list_head);
    size_t num_slots = region.bytes_committed / slot_size;

    for (size_t i = 0; i < num_slots - 1; i++) {
        void* next = static_cast<char*>(free_list_head) + (i + 1) * slot_size;
        *curr = next;
        curr = static_cast<void**>(next);
    }
    *curr = nullptr;

    total_slots = num_slots;
    allocated_slots = 0;

    valid = true;
};

bool SlabAllocator::expand() {
    std::optional<Region> re = rm.get_region(region_id);
    if (re == std::nullopt) { 
        fprintf(stderr, "SlabAllocator: region manager failed to retrieve region");
        return false;
    }

    Region region = re.value();
    size_t remaining = region.capacity - region.bytes_committed;

    if (remaining == 0) {
        fprintf(stderr, "SlabAllocator: region is full, cannot expand\n");
        return false;
    }

    // try to double
    size_t expansion = std::min(region.bytes_committed, remaining);
    
    if (!rm.expand_region(region_id, expansion)) {
        return false;
    }

    // fix up free list
    std::optional<Region> reu = rm.get_region(region_id);
    if (reu == std::nullopt) { 
        fprintf(stderr, "SlabAllocator: region manager failed to retrieve region");
        return false;;
    }

    Region updated = reu.value();
    size_t new_slots_start = updated.bytes_committed - expansion;
    char* new_base = static_cast<char*>(rm.get_absolute_addr(updated.start_offset)) + new_slots_start;
    size_t num_new_slots = expansion / slot_size;

    for (size_t i = 0; i < num_new_slots - 1; i++) {
        void* curr = new_base + i * slot_size;
        void* next = new_base + (i + 1) * slot_size;
        *static_cast<void**>(curr) = next;
    }
    // last new slot points to previous free list head
    void* last = new_base + (num_new_slots - 1) * slot_size;
    *static_cast<void**>(last) = free_list_head;
    free_list_head = new_base;

    total_slots += num_new_slots;
    return true;
}

void* SlabAllocator::allocate() {
    // grab from free list, adjust so that the head is now whatever the current head points to

    if (free_list_head == nullptr) {
        // no more free, first try to expand the region by doubling
        if (!expand()) {
            fprintf(stderr, "SlabAllocator: out of memory\n");
            return nullptr;
        }
    }    
    void* ans = free_list_head;
    void** tmp = static_cast<void**>(ans);
    free_list_head = *tmp;
    allocated_slots += 1;
    return ans;
}

void SlabAllocator::deallocate(void* addr) {
    // addr must've been returned by allocate() and belong to the current slab
    std::optional<size_t> found_id = rm.lookup_by_addr(addr);
    if (!found_id.has_value() || found_id.value() != region_id) {
        fprintf(stderr, "SlabAllocator::deallocate: address %p does not belong to this slab\n", addr);
        std::abort();
    }

    void* region_base = rm.get_absolute_addr(rm.get_region(region_id).value().start_offset);
    size_t offset_from_base = static_cast<char*>(addr) - static_cast<char*>(region_base);
    if (offset_from_base % slot_size != 0) {
        fprintf(stderr, "SlabAllocator::deallocate: address %p is not slot-aligned\n", addr);
        std::abort();
    }
    // stick it at the beginning of the free list
    void** tmp = static_cast<void**>(addr);
    *tmp = free_list_head;
    free_list_head = addr;

    allocated_slots -= 1;
}

void SlabAllocator::debug_dump() const {
    std::optional<Region> re = rm.get_region(region_id);
    if (!re.has_value()) {
        fprintf(stderr, "SlabAllocator::debug_dump: could not retrieve region\n");
        return;
    }

    const Region& region = re.value();
    void* region_base = rm.get_absolute_addr(region.start_offset);

    printf("=== SlabAllocator Debug Dump ===\n");
    printf("Region:         %s\n",   region.name);
    printf("Base address:   %p\n",   region_base);
    printf("Slot size:      %zu B\n", slot_size);
    printf("Capacity:       %zu KB\n", region.capacity / 1024);
    printf("Committed:      %zu KB\n", region.bytes_committed / 1024);
    printf("Total slots:    %zu\n",   total_slots);
    printf("Allocated:      %zu\n",   allocated_slots);
    printf("Available:      %zu\n",   total_slots - allocated_slots);
    printf("Utilization:    %.1f%%\n",
           total_slots > 0 ? (100.0 * allocated_slots / total_slots) : 0.0);
    printf("Free list head: %p\n",   free_list_head);

    // walk the free list to verify its length matches slots_available()
    size_t free_count = 0;
    void* curr = free_list_head;
    while (curr != nullptr) {
        free_count++;
        curr = *static_cast<void**>(curr);
        if (free_count > total_slots) {
            printf("WARNING: free list appears to be corrupted (cycle detected)\n");
            break;
        }
    }
    printf("Free list len:  %zu", free_count);
    if (free_count != total_slots - allocated_slots) {
        printf("  WARNING: mismatch with expected %zu", total_slots - allocated_slots);
    }
    printf("\n================================\n");
}