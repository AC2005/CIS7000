#ifndef RING_BUFFER_H_
#define RING_BUFFER_H_

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cassert>

struct Message {
    uint64_t sequence;
    uint64_t timestamp;
    uint64_t price;
    uint64_t quantity;
    char     symbol[16];
    uint8_t  type;
    uint8_t  padding[7];
};

template<size_t N>
class RingBuffer {
public:
    RingBuffer();
    bool try_push(const Message& msg);
    bool try_pop(Message& msg);
    size_t size() const;
    bool empty() const;
    bool full() const;

private:
    // head = next write position, owned by producer
    struct alignas(64) {
        std::atomic<uint64_t> value{0};
        char padding[56];
    } head;

    // tail = next read position, owned by consumer
    struct alignas(64) {
        std::atomic<uint64_t> value{0};
        char padding[56];
    } tail;

    Message arr[N];
};

template<size_t N>
RingBuffer<N>::RingBuffer() {
    // ensure capacity is power of two so mod op later is easy
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be a power of two");
}

template<size_t N>
bool RingBuffer<N>::try_push(const Message& msg) {
    // producer "owns" head (consumer doesn't write to it) so can use relaxed
    uint64_t h = head.value.load(std::memory_order_relaxed);
    // consumer writes to tail so use acquire to make sure latest writes are seen
    uint64_t t = tail.value.load(std::memory_order_acquire);
    if (h - t == N) {
        return false;
    }
    arr[h & (N - 1)] = msg;
    head.value.store(h + 1, std::memory_order_release);
    return true;
}

template<size_t N>
bool RingBuffer<N>::try_pop(Message& msg) {
    uint64_t t = tail.value.load(std::memory_order_relaxed);
    uint64_t h = head.value.load(std::memory_order_acquire);
    if (h == t) {
        return false;  // empty
    }
    msg = arr[t & (N - 1)];
    tail.value.store(t + 1, std::memory_order_release);
    return true;
}

template<size_t N>
size_t RingBuffer<N>::size() const {
    uint64_t h = head.value.load(std::memory_order_relaxed);
    uint64_t t = tail.value.load(std::memory_order_relaxed);
    return h - t;
}

template<size_t N>
bool RingBuffer<N>::empty() const {
    return size() == 0;
}

template<size_t N>
bool RingBuffer<N>::full() const {
    return size() == N;
}

#endif