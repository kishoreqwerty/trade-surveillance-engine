#include "live_book_registry.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "account_registry.hpp"
#include "live_pipeline.hpp"

using tse::api::LiveBookRegistry;
using tse::detectors::AccountRegistry;
using tse::fix::Order;
using tse::fix::OrderStatus;
using tse::fix::OrderType;
using tse::fix::Side;
using tse::ingestion::IngestionEvent;
using tse::ingestion::SpscRingBuffer;
using tse::pipeline::LivePipeline;

namespace {
Order make_new(int i, int64_t ts) {
    Order order;
    order.order_id = "O" + std::to_string(i);
    order.orig_order_id = order.order_id;
    order.account_id = "ACC-" + std::to_string(i % 5);
    order.instrument_id = "ACME";
    order.side = (i % 2 == 0) ? Side::kBuy : Side::kSell;
    order.price = 100.0 + static_cast<double>(i % 10) * 0.01;
    order.qty = 100 + i;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = ts;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    return order;
}
}  // namespace

// The actual proof this class exists for: HTTP handler threads calling
// snapshot() concurrently with the one thread mutating book state via
// run() must never race -- LivePipeline itself is documented as
// deliberately not thread-safe (live_pipeline.hpp), so this is the one
// place in Phase 9 a genuine data race could hide if the mutex around
// process()/snapshot() were missing or scoped wrong. Real concurrent
// threads, not simulated -- this is exactly the kind of claim only a
// sanitizer run can actually verify, not just a passing assertion.
TEST(LiveBookRegistry, ConcurrentSnapshotReadsDuringLiveProcessingDoNotRace) {
    constexpr int kEventCount = 3000;
    constexpr int kReaderThreads = 4;

    AccountRegistry accounts;
    LivePipeline pipeline({}, std::move(accounts));  // no detectors needed -- this test is about book/snapshot races
    SpscRingBuffer<IngestionEvent> queue(1024);
    LiveBookRegistry registry(queue, pipeline, /*alert_sink=*/nullptr);

    std::atomic<bool> producer_done{false};
    std::thread consumer_thread([&] { registry.run(producer_done); });

    std::atomic<bool> stop_readers{false};
    std::vector<std::thread> readers;
    for (int i = 0; i < kReaderThreads; ++i) {
        readers.emplace_back([&] {
            while (!stop_readers.load(std::memory_order_relaxed)) {
                // The result isn't asserted on per-call -- concurrently
                // with the producer this instrument may or may not have
                // any book state yet. What TSan is actually checking is
                // that this call and process()'s concurrent mutation of
                // the same OrderBook never race, regardless of what value
                // comes back.
                registry.snapshot("ACME");
            }
        });
    }

    std::thread producer_thread([&] {
        for (int i = 0; i < kEventCount; ++i) {
            queue.push(IngestionEvent{make_new(i, 1000 + i)});
        }
        producer_done.store(true, std::memory_order_release);
    });

    producer_thread.join();
    consumer_thread.join();
    stop_readers.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) reader.join();

    // An unpaced 3000-push producer against a 1024-capacity queue can
    // trigger the ring buffer's own drop-oldest backpressure policy (Phase
    // 3) -- expected and fine; this test's claim is about race-freedom
    // under TSan, not about zero drops (Phase 3/6 already separately prove
    // that under a paced producer). The reconciliation invariant is what
    // actually matters: every pushed event is accounted for as either
    // processed or dropped, never both, never neither.
    EXPECT_EQ(registry.events_processed() + queue.dropped_count(), static_cast<uint64_t>(kEventCount));
    EXPECT_GT(registry.events_processed(), 0u);
    auto final_snapshot = registry.snapshot("ACME");
    ASSERT_TRUE(final_snapshot.has_value());
    EXPECT_EQ(final_snapshot->sequence, registry.events_processed());
}

TEST(LiveBookRegistry, SnapshotForNeverTradedInstrumentReturnsNullopt) {
    AccountRegistry accounts;
    LivePipeline pipeline({}, std::move(accounts));
    SpscRingBuffer<IngestionEvent> queue(64);
    LiveBookRegistry registry(queue, pipeline, /*alert_sink=*/nullptr);

    EXPECT_FALSE(registry.snapshot("NEVER-TRADED").has_value());
}
