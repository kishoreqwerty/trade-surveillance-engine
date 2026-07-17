#include <gtest/gtest.h>

#include "spsc_ring_buffer.hpp"

using tse::ingestion::SpscRingBuffer;

TEST(SpscRingBuffer, RejectsNonPowerOfTwoCapacity) {
    EXPECT_THROW(SpscRingBuffer<int>(0), std::invalid_argument);
    EXPECT_THROW(SpscRingBuffer<int>(3), std::invalid_argument);
    EXPECT_THROW(SpscRingBuffer<int>(5), std::invalid_argument);
    EXPECT_THROW(SpscRingBuffer<int>(100), std::invalid_argument);
    EXPECT_THROW(SpscRingBuffer<int>(1), std::invalid_argument);  // see below, and spsc_ring_buffer.hpp
    EXPECT_NO_THROW(SpscRingBuffer<int>(2));
    EXPECT_NO_THROW(SpscRingBuffer<int>(64));
}

// capacity == 1 used to be accepted (this test asserted EXPECT_NO_THROW
// for it) but was never actually exercised with real push/pop traffic by
// any test -- only construction. It's rejected as of the Phase 6
// sequence-number rewrite: at capacity 1, the "just published index N"
// marker and the "vacated, ready for index N+1" marker are numerically
// identical (both N+1, since every index maps to the same single physical
// cell), so push() would silently overwrite unconsumed data without ever
// reclaiming it or counting it as dropped. Found by manual trace while
// building HighContentionWraparoundRegressionForBug3 below, not by a
// failing test. See cpp/ingestion/README.md.
TEST(SpscRingBuffer, CapacityOneIsRejectedNotJustDiscouraged) {
    EXPECT_THROW(SpscRingBuffer<int>(1), std::invalid_argument);
}

TEST(SpscRingBuffer, EmptyBufferPopReturnsFalse) {
    SpscRingBuffer<int> buffer(8);
    int value = -1;
    EXPECT_FALSE(buffer.pop(value));
    EXPECT_EQ(value, -1);  // untouched
}

TEST(SpscRingBuffer, SingleElementRoundTrip) {
    SpscRingBuffer<int> buffer(8);
    EXPECT_TRUE(buffer.push(42));
    int value = 0;
    ASSERT_TRUE(buffer.pop(value));
    EXPECT_EQ(value, 42);
    EXPECT_FALSE(buffer.pop(value));  // drained
}

TEST(SpscRingBuffer, PreservesFifoOrderUnderCapacity) {
    SpscRingBuffer<int> buffer(8);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(buffer.push(i));
    }
    for (int i = 0; i < 5; ++i) {
        int value = -1;
        ASSERT_TRUE(buffer.pop(value));
        EXPECT_EQ(value, i);
    }
    EXPECT_EQ(buffer.dropped_count(), 0u);
}

TEST(SpscRingBuffer, InterleavedPushPopStaysInOrder) {
    SpscRingBuffer<int> buffer(4);
    int value = -1;

    EXPECT_TRUE(buffer.push(1));
    EXPECT_TRUE(buffer.push(2));
    ASSERT_TRUE(buffer.pop(value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(buffer.push(3));
    EXPECT_TRUE(buffer.push(4));
    ASSERT_TRUE(buffer.pop(value));
    EXPECT_EQ(value, 2);
    ASSERT_TRUE(buffer.pop(value));
    EXPECT_EQ(value, 3);
    ASSERT_TRUE(buffer.pop(value));
    EXPECT_EQ(value, 4);
    EXPECT_FALSE(buffer.pop(value));
}

TEST(SpscRingBuffer, WrapsAroundCorrectlyAcrossManyCycles) {
    SpscRingBuffer<int> buffer(4);
    // Push/pop far more than capacity to force many index wraparounds
    // (head_/tail_ are monotonic and only masked when indexing storage —
    // this exercises that masking is correct across repeated wraps).
    for (int cycle = 0; cycle < 1000; ++cycle) {
        EXPECT_TRUE(buffer.push(cycle));
        int value = -1;
        ASSERT_TRUE(buffer.pop(value));
        EXPECT_EQ(value, cycle);
    }
    EXPECT_EQ(buffer.dropped_count(), 0u);
}

TEST(SpscRingBuffer, PushBeyondCapacityDropsOldestSingleThreaded) {
    SpscRingBuffer<int> buffer(4);
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(buffer.push(i));  // fills 0,1,2,3

    // Buffer is full (4/4). This push must drop the oldest (0) to make
    // room for 4.
    EXPECT_FALSE(buffer.push(4));
    EXPECT_EQ(buffer.dropped_count(), 1u);

    int value = -1;
    ASSERT_TRUE(buffer.pop(value));
    EXPECT_EQ(value, 1);  // 0 was dropped; 1 is now the oldest survivor
    ASSERT_TRUE(buffer.pop(value));
    EXPECT_EQ(value, 2);
    ASSERT_TRUE(buffer.pop(value));
    EXPECT_EQ(value, 3);
    ASSERT_TRUE(buffer.pop(value));
    EXPECT_EQ(value, 4);
    EXPECT_FALSE(buffer.pop(value));
}

TEST(SpscRingBuffer, SustainedOverflowDropsExactlyTheExpectedCount) {
    SpscRingBuffer<int> buffer(4);
    // Push 100 elements into a 4-slot buffer with no draining: exactly
    // 96 of them must be dropped, leaving the newest 4 (96,97,98,99).
    for (int i = 0; i < 100; ++i) buffer.push(i);

    EXPECT_EQ(buffer.dropped_count(), 96u);
    EXPECT_EQ(buffer.size_approx(), 4u);

    int value = -1;
    for (int expected = 96; expected < 100; ++expected) {
        ASSERT_TRUE(buffer.pop(value));
        EXPECT_EQ(value, expected);
    }
    EXPECT_FALSE(buffer.pop(value));
}

TEST(SpscRingBuffer, SizeApproxTracksPushesAndPops) {
    SpscRingBuffer<int> buffer(16);
    EXPECT_EQ(buffer.size_approx(), 0u);
    for (int i = 0; i < 5; ++i) buffer.push(i);
    EXPECT_EQ(buffer.size_approx(), 5u);
    int value;
    buffer.pop(value);
    buffer.pop(value);
    EXPECT_EQ(buffer.size_approx(), 3u);
}

TEST(SpscRingBuffer, CapacityReportsConstructedValue) {
    SpscRingBuffer<int> buffer(128);
    EXPECT_EQ(buffer.capacity(), 128u);
}

TEST(SpscRingBuffer, WorksWithNonTrivialMovableType) {
    SpscRingBuffer<std::string> buffer(4);
    EXPECT_TRUE(buffer.push("alpha"));
    EXPECT_TRUE(buffer.push("beta"));
    std::string value;
    ASSERT_TRUE(buffer.pop(value));
    EXPECT_EQ(value, "alpha");
    ASSERT_TRUE(buffer.pop(value));
    EXPECT_EQ(value, "beta");
}
