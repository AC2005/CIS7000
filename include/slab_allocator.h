#ifndef SLAB_ALLOCATOR_H_
#define SLAB_ALLOCATOR_H_

#include "virtual_memory_manager.h"

class SlabAllocator {
    public:
        SlabAllocator(RegionManager &rm, const char* name, size_t capacity, size_t slot_size);
        void* allocate();
        void deallocate(void* addr);
        bool expand();

        size_t slots_available() { return total_slots - allocated_slots; }
        size_t slots_total() { return total_slots; }

        void debug_dump() const;
        bool is_valid() const { return valid; }
    private:
        RegionManager &rm;
        size_t region_id;
        size_t slot_size;
        size_t total_slots;
        size_t allocated_slots;

        void* free_list_head;
        bool valid = false;
        
};

#endif