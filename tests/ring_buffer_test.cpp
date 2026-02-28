#include "ring_buffer.h"
#include <cstdio>
#include <cassert>
#include <thread>
#include <atomic>
#include <cstring>

// -------------------------------------------------------
// Helpers
// -------------------------------------------------------

Message make_message(uint64_t seq, uint64_t price = 100, const char* symbol = "AAPL") {
    Message m{};
    m.sequence  = seq;
    m.timestamp = seq * 10;
    m.price     = price;
    m.quantity  = 10;
    m.type      = 1;
    strncpy(m.symbol, symbol, sizeof(m.symbol) - 1);
    return m;
}

// -------------------------------------------------------
// Single-threaded tests
// -------------------------------------------------------

void test_empty_on_construction() {
    printf("--- test_empty_on_construction ---\n");
    RingBuffer<16> rb;
    assert(rb.empty());
    assert(!rb.full());
    assert(rb.size() == 0);
    printf("PASS\n");
}

void test_single_push_pop() {
    printf("--- test_single_push_pop ---\n");
    RingBuffer<16> rb;

    Message sent = make_message(1);
    assert(rb.try_push(sent));
    assert(rb.size() == 1);
    assert(!rb.empty());

    Message recv{};
    assert(rb.try_pop(recv));
    assert(recv.sequence == sent.sequence);
    assert(recv.price    == sent.price);
    assert(strncmp(recv.symbol, sent.symbol, sizeof(sent.symbol)) == 0);
    assert(rb.empty());
    printf("PASS\n");
}

void test_fill_and_drain() {
    printf("--- test_fill_and_drain ---\n");
    constexpr size_t CAP = 8;
    RingBuffer<CAP> rb;

    // fill to capacity
    for (size_t i = 0; i < CAP; i++) {
        assert(rb.try_push(make_message(i)));
    }
    assert(rb.full());
    assert(rb.size() == CAP);

    // one more push should fail
    assert(!rb.try_push(make_message(99)));

    // drain and verify order
    for (size_t i = 0; i < CAP; i++) {
        Message m{};
        assert(rb.try_pop(m));
        assert(m.sequence == i && "messages must come out in order");
    }
    assert(rb.empty());

    // one more pop should fail
    Message m{};
    assert(!rb.try_pop(m));
    printf("PASS\n");
}

void test_wrap_around() {
    printf("--- test_wrap_around ---\n");
    constexpr size_t CAP = 4;
    RingBuffer<CAP> rb;

    // push 2, pop 2, push 4 — forces indices to wrap
    for (size_t i = 0; i < 2; i++) rb.try_push(make_message(i));
    for (size_t i = 0; i < 2; i++) { Message m{}; rb.try_pop(m); }
    for (size_t i = 2; i < 6; i++) assert(rb.try_push(make_message(i)));

    // verify all 4 come out correctly
    for (size_t i = 2; i < 6; i++) {
        Message m{};
        assert(rb.try_pop(m));
        assert(m.sequence == i);
    }
    assert(rb.empty());
    printf("PASS\n");
}

void test_interleaved_push_pop() {
    printf("--- test_interleaved_push_pop ---\n");
    RingBuffer<16> rb;

    // push 5, pop 3, push 5, pop 7 — verify sequence throughout
    uint64_t send_seq = 0;
    uint64_t recv_seq = 0;

    auto push_n = [&](size_t n) {
        for (size_t i = 0; i < n; i++) {
            assert(rb.try_push(make_message(send_seq++)));
        }
    };
    auto pop_n = [&](size_t n) {
        for (size_t i = 0; i < n; i++) {
            Message m{};
            assert(rb.try_pop(m));
            assert(m.sequence == recv_seq++ && "sequence mismatch");
        }
    };

    push_n(5);
    pop_n(3);
    push_n(5);
    pop_n(7);
    assert(rb.empty());
    printf("PASS\n");
}

// -------------------------------------------------------
// Multi-threaded tests
// -------------------------------------------------------

void test_spsc_correctness(size_t num_messages) {
    printf("--- test_spsc_correctness (%zu messages) ---\n", num_messages);
    RingBuffer<1024> rb;

    std::atomic<bool> done{false};
    std::atomic<uint64_t> errors{0};

    std::thread producer([&]() {
        for (size_t i = 0; i < num_messages; i++) {
            Message m = make_message(i);
            // spin until space is available
            while (!rb.try_push(m)) {}
        }
    });

    std::thread consumer([&]() {
        uint64_t expected_seq = 0;
        while (expected_seq < num_messages) {
            Message m{};
            if (rb.try_pop(m)) {
                if (m.sequence != expected_seq) {
                    errors++;
                    fprintf(stderr, "sequence error: expected %zu got %zu\n",
                            (size_t)expected_seq, (size_t)m.sequence);
                }
                expected_seq++;
            }
        }
    });

    producer.join();
    consumer.join();

    assert(errors == 0 && "sequence errors detected — possible memory ordering bug");
    assert(rb.empty());
    printf("PASS\n");
}

void test_spsc_no_drops(size_t num_messages) {
    printf("--- test_spsc_no_drops (%zu messages) ---\n", num_messages);
    RingBuffer<1024> rb;

    std::atomic<uint64_t> total_sent{0};
    std::atomic<uint64_t> total_recv{0};

    std::thread producer([&]() {
        for (size_t i = 0; i < num_messages; i++) {
            while (!rb.try_push(make_message(i))) {}
            total_sent++;
        }
    });

    std::thread consumer([&]() {
        uint64_t count = 0;
        while (count < num_messages) {
            Message m{};
            if (rb.try_pop(m)) {
                count++;
                total_recv++;
            }
        }
    });

    producer.join();
    consumer.join();

    assert(total_sent == num_messages);
    assert(total_recv == num_messages);
    assert(total_sent == total_recv && "message count mismatch — drops detected");
    printf("PASS\n");
}

void test_spsc_data_integrity() {
    printf("--- test_spsc_data_integrity ---\n");
    constexpr size_t NUM = 100000;
    RingBuffer<256> rb;
    std::atomic<uint64_t> errors{0};

    std::thread producer([&]() {
        for (size_t i = 0; i < NUM; i++) {
            Message m = make_message(i, i * 10);  // price = seq * 10
            while (!rb.try_push(m)) {}
        }
    });

    std::thread consumer([&]() {
        uint64_t expected = 0;
        while (expected < NUM) {
            Message m{};
            if (rb.try_pop(m)) {
                // verify both fields are consistent — catch torn reads
                if (m.sequence != expected || m.price != expected * 10) {
                    errors++;
                    fprintf(stderr, "data integrity error at seq %zu: price=%zu expected=%zu\n",
                            (size_t)m.sequence, (size_t)m.price, (size_t)(expected * 10));
                }
                expected++;
            }
        }
    });

    producer.join();
    consumer.join();

    assert(errors == 0 && "data integrity errors — possible torn reads");
    printf("PASS\n");
}

// -------------------------------------------------------
// Main
// -------------------------------------------------------

int main() {
    printf("=== RingBuffer Functionality Tests ===\n\n");

    printf("-- Single-threaded --\n");
    test_empty_on_construction();
    test_single_push_pop();
    test_fill_and_drain();
    test_wrap_around();
    test_interleaved_push_pop();

    printf("\n-- Multi-threaded --\n");
    test_spsc_correctness(1'000'000);
    test_spsc_no_drops(1'000'000);
    test_spsc_data_integrity();

    printf("\nAll tests passed.\n");
    return 0;
}