#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "spsc_ring_buffer.hpp"

using tse::ingestion::SpscRingBuffer;

// Backpressure policy: drop-oldest (see spsc_ring_buffer.hpp's header
// comment and cpp/ingestion/README.md for the full justification — briefly:
// blocking the producer risks starving Phase 2's FIX session heartbeats,
// and growing contradicts the fixed-capacity requirement and can't be done
// lock-free). These tests exercise that policy directly by deliberately
// never letting a consumer run while the producer keeps pushing.

TEST(Backpressure, StalledConsumerCausesDropOldestNotBlockOrGrow) {
    SpscRingBuffer<int> buffer(8);

    // The consumer never runs at all here — this is the "stall" — while
    // the producer keeps pushing well past capacity on this single thread.
    // If the policy were "block", this loop would hang; if it were "grow",
    // dropped_count() would stay 0 and capacity() would change. Neither
    // happens.
    for (int i = 0; i < 1000; ++i) {
        buffer.push(i);
    }

    EXPECT_EQ(buffer.capacity(), 8u);  // never grew
    EXPECT_EQ(buffer.dropped_count(), 1000u - 8u);
    EXPECT_EQ(buffer.size_approx(), 8u);

    // Once the "consumer" finally does run, it sees a coherent (if gapped)
    // suffix: the 8 most recent pushes, in order, nothing else.
    int value = -1;
    for (int expected = 992; expected < 1000; ++expected) {
        ASSERT_TRUE(buffer.pop(value));
        EXPECT_EQ(value, expected);
    }
    EXPECT_FALSE(buffer.pop(value));
}

// Same policy, but with a genuine second thread deliberately stalled (put
// to sleep) while a producer thread keeps pushing concurrently — closer to
// the real pipeline shape (order-book-updater thread momentarily unable to
// keep up) than the single-threaded version above, and gives TSan another
// concurrent drop-oldest scenario to check under tsan_suppressions.txt.
TEST(Backpressure, DeliberatelyStalledConsumerThreadStillObservesDropOldest) {
    constexpr std::size_t kCapacity = 16;
    constexpr int kTotalPushes = 5000;

    SpscRingBuffer<int> buffer(kCapacity);
    std::atomic<bool> consumer_may_start{false};
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (int i = 0; i < kTotalPushes; ++i) {
            buffer.push(i);
        }
        producer_done.store(true, std::memory_order_release);
        consumer_may_start.store(true, std::memory_order_release);
    });

    // The consumer thread exists but is deliberately held back — it does
    // not call pop() at all until the producer has already finished
    // pushing everything, guaranteeing every push after the buffer first
    // fills has to go through the drop-oldest path rather than racing a
    // draining consumer.
    std::vector<int> received;
    std::thread consumer([&] {
        while (!consumer_may_start.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        int value = -1;
        while (buffer.pop(value)) {
            received.push_back(value);
        }
    });

    producer.join();
    consumer.join();

    ASSERT_TRUE(producer_done.load());
    EXPECT_EQ(buffer.capacity(), kCapacity);  // never grew
    EXPECT_EQ(static_cast<int>(received.size()) + static_cast<int>(buffer.dropped_count()), kTotalPushes);
    EXPECT_EQ(received.size(), kCapacity);  // only the last kCapacity survive a fully-stalled consumer

    // The survivors are exactly the newest kCapacity pushes, in order.
    for (std::size_t i = 0; i < received.size(); ++i) {
        EXPECT_EQ(received[i], kTotalPushes - static_cast<int>(kCapacity) + static_cast<int>(i));
    }
}
