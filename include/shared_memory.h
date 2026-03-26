#ifndef SHARED_MEMORY_H_
#define SHARED_MEMORY_H_

#include <cstddef>
class SharedMemory {
public:
    static SharedMemory create(const char* name, size_t size);
    static SharedMemory attach(const char* name, size_t size);

    ~SharedMemory();

    // make it moveable but not copyable
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    SharedMemory(SharedMemory&& other);
    SharedMemory& operator=(SharedMemory&& other);

    void*  get_ptr()    const { return ptr; }
    size_t get_size()   const { return size; }
    bool   is_valid()   const { return valid; }
    bool   is_creator() const { return is_owner; }

    void cleanup();
    void debug_dump() const;

private:
    // only want to expose attach and create func
    SharedMemory(const char* name, size_t size, bool owner, void* ptr);

    char   name[64];
    void*  ptr;
    size_t size;
    bool   is_owner;
    bool   valid;
};

#endif