// shared_memory.cpp
#include "shared_memory.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>

// retry timing for attach
static constexpr int    ATTACH_RETRIES_MS  = 100;  
static constexpr int    ATTACH_SLEEP_MS    = 10;

SharedMemory::SharedMemory(const char* name, size_t size, bool owner, void* ptr)
    : ptr(ptr), size(size), is_owner(owner), valid(true)
{
    strncpy(this->name, name, sizeof(this->name) - 1);
    this->name[sizeof(this->name) - 1] = '\0';
}

SharedMemory SharedMemory::create(const char* name, size_t size) {
    shm_unlink(name);

    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("SharedMemory::create: shm_open failed");
        std::abort();
    }

    if (ftruncate(fd, (off_t)size) == -1) {
        perror("SharedMemory::create: ftruncate failed");
        close(fd);
        shm_unlink(name);
        std::abort();
    }

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("SharedMemory::create: mmap failed");
        close(fd);
        shm_unlink(name);
        std::abort();
    }

    // fd no longer needed once mapped
    close(fd);

    return SharedMemory(name, size, /*owner=*/true, ptr);
}

SharedMemory SharedMemory::attach(const char* name, size_t size) {
    int fd = -1;

    // retry to ensure creator has starter
    for (int i = 0; i < ATTACH_RETRIES_MS; i++) {
        fd = shm_open(name, O_RDWR, 0);
        if (fd != -1) break;

        if (errno != ENOENT) {
            perror("SharedMemory::attach: shm_open failed");
            std::abort();
        }

        fprintf(stderr, "SharedMemory::attach: waiting for '%s' to be created... (%d/%d)\n",
                name, i + 1, ATTACH_RETRIES_MS);
        usleep(ATTACH_SLEEP_MS * 1000);
    }

    if (fd == -1) {
        fprintf(stderr, "SharedMemory::attach: timed out waiting for '%s'\n", name);
        std::abort();
    }

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("SharedMemory::attach: mmap failed");
        close(fd);
        std::abort();
    }

    close(fd);

    return SharedMemory(name, size, /*owner=*/false, ptr);
}

SharedMemory::~SharedMemory() {
    cleanup();
}

void SharedMemory::cleanup() {
    if (!valid) return;

    if (ptr != nullptr && ptr != MAP_FAILED) {
        if (munmap(ptr, size) == -1) {
            perror("SharedMemory::cleanup: munmap failed");
        }
        ptr = nullptr;
    }

    if (is_owner) {
        if (shm_unlink(name) == -1) {
            // not fatal — may have already been unlinked
            perror("SharedMemory::cleanup: shm_unlink failed");
        }
    }

    valid = false;
}

SharedMemory::SharedMemory(SharedMemory&& other)
    : ptr(other.ptr)
    , size(other.size)
    , is_owner(other.is_owner)
    , valid(other.valid)
{
    strncpy(name, other.name, sizeof(name));
    // zero out source so its destructor is a no-op
    other.ptr      = nullptr;
    other.valid    = false;
    other.is_owner = false;
}

SharedMemory& SharedMemory::operator=(SharedMemory&& other) {
    if (this == &other) return *this;

    // clean up what we currently own before taking on new resource
    cleanup();

    ptr      = other.ptr;
    size     = other.size;
    is_owner = other.is_owner;
    valid    = other.valid;
    strncpy(name, other.name, sizeof(name));

    other.ptr      = nullptr;
    other.valid    = false;
    other.is_owner = false;

    return *this;
}

void SharedMemory::debug_dump() const {
    printf("=== SharedMemory Debug Dump ===\n");
    printf("Name:     %s\n",   name);
    printf("Ptr:      %p\n",   ptr);
    printf("Size:     %zu KB\n", size / 1024);
    printf("Owner:    %s\n",   is_owner ? "yes (will shm_unlink)" : "no");
    printf("Valid:    %s\n",   valid    ? "yes" : "no");
    printf("Path:     /dev/shm%s\n", name);
    printf("===============================\n");
}