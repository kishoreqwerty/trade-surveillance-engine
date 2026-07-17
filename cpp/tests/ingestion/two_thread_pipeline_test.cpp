#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "ingestion_event.hpp"
#include "spsc_ring_buffer.hpp"

using namespace tse::fix;
using tse::ingestion::IngestionEvent;
using tse::ingestion::SpscRingBuffer;

namespace {

// Builds the Nth order the "FIX parser" producer thread would hand off —
// the sequence number is encoded in `qty` so the consumer can verify strict
// ordering and detect any duplication/corruption without needing a full
// order book (Phase 4 doesn't exist yet; the consumer role here is a
// stand-in, not a real book).
Order make_sequenced_order(int64_t seq) {
    Order order;
    order.order_id = "ORD-" + std::to_string(seq);
    order.account_id = "ACC-1";
    order.instrument_id = "ACME";
    order.side = (seq % 2 == 0) ? Side::kBuy : Side::kSell;
    order.price = 100.0;
    order.qty = seq;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = seq;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    return order;
}

}  // namespace

// This is the test cpp/ingestion/README.md and CLAUDE.md's TSan mandate
// point at: a genuine second thread (producer role = FIX parser handing
// off parsed Order events, consumer role = a stand-in for the order book
// updater — Phase 4's real order book doesn't exist yet) hammering the same
// SpscRingBuffer under sustained load, including the drop-oldest reclaim
// path. Run under TSan via tsan_suppressions.txt (QuickFIX-internal noise
// suppressed, nothing in this file/spsc_ring_buffer.hpp is).
TEST(TwoThreadPipeline, SustainedLoadPreservesOrderingWithNoLossOrCorruption) {
    constexpr int64_t kTotalEvents = 100'000;
    constexpr std::size_t kCapacity = 1024;

    SpscRingBuffer<IngestionEvent> buffer(kCapacity);
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (int64_t seq = 0; seq < kTotalEvents; ++seq) {
            buffer.push(make_sequenced_order(seq));
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::vector<int64_t> received;
    received.reserve(kTotalEvents);

    std::thread consumer([&] {
        // Deliberate head start for the producer: this is what makes the
        // drop-oldest path *deterministically* exercised rather than
        // relying on the producer simply being faster than the consumer.
        // That relative-speed assumption held under a plain benchmark
        // build but silently didn't under TSan (instrumentation overhead
        // doesn't scale both threads' relative speed uniformly) — this
        // test flaked exactly there before this fix. 100,000 pushes against
        // a 1024-capacity buffer take well under 20ms even instrumented, so
        // this sleep reliably lets the producer overflow the buffer many
        // times before the consumer ever calls pop(), independent of which
        // build config or machine this runs on.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        IngestionEvent event;
        while (true) {
            if (buffer.pop(event)) {
                received.push_back(std::get<Order>(event).qty);
                continue;
            }
            if (producer_done.load(std::memory_order_acquire)) {
                // One more drain attempt: the producer could have pushed
                // its very last element between our failed pop() above and
                // observing producer_done here.
                if (buffer.pop(event)) {
                    received.push_back(std::get<Order>(event).qty);
                    continue;
                }
                break;
            }
            std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();

    // Core correctness invariant: every pushed event is accounted for
    // exactly once, either delivered or dropped — never both, never
    // neither.
    EXPECT_EQ(static_cast<int64_t>(received.size()) + static_cast<int64_t>(buffer.dropped_count()),
              kTotalEvents);

    // Whatever the consumer *did* receive must be in strictly increasing
    // sequence order with no duplicates and no garbage values — this is
    // what would catch a torn read or a lost synchronizes-with edge in the
    // drop-oldest CAS protocol (a corrupted claim could hand the consumer
    // a value from the wrong slot, or the same slot twice).
    for (std::size_t i = 1; i < received.size(); ++i) {
        ASSERT_GT(received[i], received[i - 1]) << "ordering violated at index " << i;
    }
    for (int64_t value : received) {
        ASSERT_GE(value, 0);
        ASSERT_LT(value, kTotalEvents);
    }

    // Sustained load under a small capacity should force the drop-oldest
    // path to actually run, not just the fast path — otherwise this test
    // wouldn't be exercising the concurrent claim protocol at all.
    EXPECT_GT(buffer.dropped_count(), 0u);
}

// Same sustained-load shape but with the consumer processing every event
// (small capacity, producer paced to roughly keep up) so the fast
// (non-dropping) path also gets real concurrent TSan coverage, not just the
// drop-oldest path exercised above.
TEST(TwoThreadPipeline, SustainedLoadWithConsumerKeepingUpDropsNothing) {
    constexpr int64_t kTotalEvents = 50'000;
    constexpr std::size_t kCapacity = 4096;

    SpscRingBuffer<IngestionEvent> buffer(kCapacity);
    std::atomic<bool> producer_done{false};
    std::atomic<int64_t> pushed{0};

    std::vector<int64_t> received;
    received.reserve(kTotalEvents);

    std::thread consumer([&] {
        IngestionEvent event;
        while (true) {
            if (buffer.pop(event)) {
                received.push_back(std::get<Order>(event).qty);
                continue;
            }
            if (producer_done.load(std::memory_order_acquire)) {
                if (buffer.pop(event)) {
                    received.push_back(std::get<Order>(event).qty);
                    continue;
                }
                break;
            }
            std::this_thread::yield();
        }
    });

    std::thread producer([&] {
        for (int64_t seq = 0; seq < kTotalEvents; ++seq) {
            // Backpressure at the producer, not the queue: keep the queue
            // mostly non-full by pacing pushes against the consumer's
            // progress, so this run exercises the plain (non-dropping)
            // concurrent path specifically.
            while (static_cast<int64_t>(buffer.size_approx()) > static_cast<int64_t>(kCapacity) / 2) {
                std::this_thread::yield();
            }
            buffer.push(make_sequenced_order(seq));
            pushed.fetch_add(1, std::memory_order_relaxed);
        }
        producer_done.store(true, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(buffer.dropped_count(), 0u);
    ASSERT_EQ(received.size(), static_cast<std::size_t>(kTotalEvents));
    for (int64_t i = 0; i < kTotalEvents; ++i) {
        EXPECT_EQ(received[static_cast<std::size_t>(i)], i);
    }
}
