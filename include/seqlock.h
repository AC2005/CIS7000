#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>

template<typename T>
class Seqlock {
public:
    Seqlock() : seq(0) {}

    // write a new value — called by the single writer
    void write(const T& new_val) {
        uint64_t s = seq.load(std::memory_order_relaxed);

        // seq must be even before we start — if odd something is wrong
        seq.store(s + 1, std::memory_order_release);  // odd: write in progress

        data = new_val;

        seq.store(s + 2, std::memory_order_release);  // even: write complete
    }

    // read a consistent snapshot
    T read() const {
        T snapshot;
        uint64_t s1, s2;

        do {
            s1 = seq.load(std::memory_order_acquire);
            if (s1 & 1) continue;  // odd = write in progress, spin

            std::atomic_thread_fence(std::memory_order_acquire);
            snapshot = data;
            std::atomic_thread_fence(std::memory_order_acquire);

            s2 = seq.load(std::memory_order_acquire);
        } while (s1 != s2);

        return snapshot;
    }

    // try to read — returns false if a write is in progress
    bool try_read(T& out) const {
        uint64_t s1 = seq.load(std::memory_order_acquire);
        if (s1 & 1) return false;  // write in progress

        std::atomic_thread_fence(std::memory_order_acquire);
        out = data;
        std::atomic_thread_fence(std::memory_order_acquire);

        uint64_t s2 = seq.load(std::memory_order_acquire);
        return s1 == s2;
    }

    uint64_t get_seq() const {
        return seq.load(std::memory_order_relaxed);
    }

    void reset() {
        seq.store(0, std::memory_order_seq_cst);
        data = T{};
    }
private:
    // seq and data on separate cache lines to prevent false sharing
    alignas(64) std::atomic<uint64_t> seq;
    char seq_padding[56];
    alignas(64) T data;
};