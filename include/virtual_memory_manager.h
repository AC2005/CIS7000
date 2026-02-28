#ifndef VIRTUAL_MEMORY_MANAGER_H
#define VIRTUAL_MEMORY_MANAGER_H

#include <cstdlib>
#include <stdint.h>
#include <optional>
#include <array>
#include <tuple>
#include <signal.h>
#include <execinfo.h>

class VirtualAddressSpace {
    public:
        VirtualAddressSpace(size_t mem_size);
        ~VirtualAddressSpace();

        void commit(size_t offset, size_t size);
        void uncommit(size_t offset, size_t size);

        void* get_base_addr() const {
            return base_addr;
        }
        size_t get_mem_size() const {
            return mem_size;
        }
        bool check_valid_addr(void* addr) const {
            uintptr_t a = reinterpret_cast<uintptr_t>(addr);
            uintptr_t base = reinterpret_cast<uintptr_t>(base_addr);
            return (a >= base && a < a + mem_size);
        }
    private:
        void* base_addr;
        size_t mem_size;

        VirtualAddressSpace(const VirtualAddressSpace&) = delete;
        VirtualAddressSpace& operator=(const VirtualAddressSpace&) = delete;
};

/*

A region is a fixed size section of the underlying virtual address space.
Each region has it's own allocator (e.g slab). The allocator can ask for 
more memory, in which case the region manager will commit more pages in the
virtual address space.

For my application, we'll always know exactly how many regions are needed, so 
we can have an array of static size of regions.
*/

constexpr size_t MAX_REGIONS = 16;

enum class AllocatorType {
    SLAB,
    BUMP
};

enum class FaultType {
    UNDERFLOW,
    OVERFLOW,
    NO_FAULT
};

struct Region{
    char name[32];
    size_t start_offset;
    size_t capacity;
    size_t bytes_committed;
    AllocatorType allocator;
};

class RegionManager {
    public:
        RegionManager(VirtualAddressSpace& vas) : vas(vas), end_offset(0), region_count(0) {};
        ~RegionManager() {};

        std::optional<size_t> allocate_region(const char* name, size_t capacity, AllocatorType allocator);
        bool expand_region(size_t region_id, size_t num_bytes);
        std::optional<size_t> lookup_by_addr(void* addr);
        std::tuple<FaultType, std::optional<size_t>> is_guard_page(void* addr);

        std::optional<Region> get_region(size_t i) {
            if (i >= region_count) {
                return std::nullopt;
            }
            return regions[i];
        }

        void* get_absolute_addr(size_t offset) {
            return static_cast<char*>(vas.get_base_addr()) + offset;
        }
        // debug functions
        std::optional<size_t> lookup_by_name(const char* name);
        void debug_dump() const;
        
        std::array<Region, MAX_REGIONS> regions;
    private:
        VirtualAddressSpace& vas;
        size_t end_offset;
        size_t region_count;
        
};

#endif