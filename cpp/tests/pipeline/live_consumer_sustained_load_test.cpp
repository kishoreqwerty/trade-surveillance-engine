#include "live_consumer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "alert_sink.hpp"
#include "front_running_detector.hpp"
#include "live_pipeline.hpp"
#include "marking_the_close_detector.hpp"
#include "simulator.hpp"
#include "spoofing_layering_detector.hpp"
#include "statistical_baseline_detector.hpp"
#include "wash_trade_detector.hpp"

using tse::detectors::AccountRegistry;
using tse::detectors::Entity;
using tse::detectors::FrontRunningDetector;
using tse::detectors::IDetector;
using tse::detectors::MarkingTheCloseConfig;
using tse::detectors::MarkingTheCloseDetector;
using tse::detectors::SpoofingLayeringDetector;
using tse::detectors::StatisticalBaselineDetector;
using tse::detectors::WashTradeDetector;
using tse::ingestion::IngestionEvent;
using tse::ingestion::SpscRingBuffer;
using tse::pipeline::CollectingAlertSink;
using tse::pipeline::LiveConsumer;
using tse::pipeline::LivePipeline;

namespace {

Entity to_entity(const tse::simulator::Account& account) {
    Entity entity;
    entity.account_id = account.account_id;
    entity.beneficial_owner_id = account.beneficial_owner_id;
    entity.entity_type = tse::simulator::to_string(account.entity_type);
    entity.linked_account_ids = account.linked_account_ids;
    return entity;
}

tse::fix::Order to_fix_order(const tse::simulator::Order& order) {
    tse::fix::Order out;
    out.order_id = order.order_id;
    out.account_id = order.account_id;
    out.instrument_id = order.instrument_id;
    out.side = order.side == tse::simulator::Side::kBuy ? tse::fix::Side::kBuy : tse::fix::Side::kSell;
    out.price = order.price;
    out.qty = order.qty;
    out.order_type =
        order.order_type == tse::simulator::OrderType::kMarket ? tse::fix::OrderType::kMarket : tse::fix::OrderType::kLimit;
    out.timestamp_ns = order.timestamp_ns;
    out.status =
        order.status == tse::simulator::OrderStatus::kCancelled ? tse::fix::OrderStatus::kCancelled : tse::fix::OrderStatus::kNew;
    out.venue = order.venue;
    out.orig_order_id = order.order_id;  // simulator doesn't track a separate cancel-request id
    return out;
}

tse::fix::Execution to_fix_execution(const tse::simulator::Execution& execution, tse::fix::Side side) {
    tse::fix::Execution out;
    out.trade_id = execution.trade_id;
    out.order_id = execution.order_id;
    out.account_id = execution.account_id;
    out.instrument_id = execution.instrument_id;
    out.side = side;
    out.price = execution.price;
    out.qty = execution.qty;
    out.timestamp_ns = execution.timestamp_ns;
    out.counterparty_account_id = execution.counterparty_account_id;
    out.venue = execution.venue;
    return out;
}

int64_t timestamp_of(const IngestionEvent& event) {
    return std::visit([](const auto& concrete) { return concrete.timestamp_ns; }, event);
}

std::vector<std::unique_ptr<IDetector>> make_all_five_detectors(
    const tse::simulator::SimulationOutput& simulation) {
    std::vector<std::unique_ptr<IDetector>> result;
    result.push_back(std::make_unique<WashTradeDetector>());
    result.push_back(std::make_unique<SpoofingLayeringDetector>());

    MarkingTheCloseConfig mtc_config;
    for (const auto& instrument : simulation.instruments) {
        mtc_config.close_time_ns_by_instrument[instrument.instrument_id] = instrument.session_close_ns;
    }
    result.push_back(std::make_unique<MarkingTheCloseDetector>(mtc_config));
    result.push_back(std::make_unique<FrontRunningDetector>());
    result.push_back(std::make_unique<StatisticalBaselineDetector>());
    return result;
}

int64_t percentile_ns(const std::vector<int64_t>& sorted_values, double p) {
    if (sorted_values.empty()) return 0;
    auto idx = static_cast<size_t>(p * static_cast<double>(sorted_values.size() - 1));
    return sorted_values[idx];
}

double mean_ns(const std::vector<int64_t>& values) {
    if (values.empty()) return 0.0;
    long double sum = 0;
    for (int64_t v : values) sum += static_cast<long double>(v);
    return static_cast<double>(sum / static_cast<long double>(values.size()));
}

// A realistic multi-instrument, multi-abuse-pattern synthetic session sized
// to Phase 3's 100k-event sustained-load standard, merged from the
// simulator's separately-time-sorted Order/Execution vectors into one
// globally chronological stream -- a real exchange emits one interleaved
// stream, and an Execution must never precede the New it references.
// Orders are placed in the pre-sort vector before executions, deliberately,
// so stable_sort's tie-breaking on equal timestamp_ns keeps a New ahead of
// anything referencing it at the same instant.
struct Scenario {
    tse::simulator::SimulationOutput simulation;
    std::vector<IngestionEvent> events;
};

Scenario build_scenario(uint64_t random_seed) {
    tse::simulator::SimulatorConfig config;
    config.random_seed = random_seed;
    config.session_duration_ns = 6LL * 3600 * 1'000'000'000;  // 6h synthetic session
    config.baseline_orders_per_second = 5.0;
    config.num_equity_instruments = 3;
    config.num_fx_instruments = 2;
    config.num_fixed_income_instruments = 2;
    config.num_independent_accounts = 40;
    config.num_linked_account_pairs = 8;
    config.wash_trade = {10, 0.6};
    config.spoofing_layering = {10, 0.6};
    config.marking_the_close = {5, 0.6};
    config.front_running = {10, 0.6};

    Scenario scenario;
    scenario.simulation = tse::simulator::generate_simulation(config);

    std::unordered_map<std::string, tse::fix::Side> side_by_order_id;
    for (const auto& order : scenario.simulation.orders) {
        side_by_order_id[order.order_id] =
            order.side == tse::simulator::Side::kBuy ? tse::fix::Side::kBuy : tse::fix::Side::kSell;
    }

    scenario.events.reserve(scenario.simulation.orders.size() + scenario.simulation.executions.size());
    for (const auto& order : scenario.simulation.orders) scenario.events.push_back(to_fix_order(order));
    for (const auto& execution : scenario.simulation.executions) {
        scenario.events.push_back(to_fix_execution(execution, side_by_order_id.at(execution.order_id)));
    }
    std::stable_sort(scenario.events.begin(), scenario.events.end(),
                      [](const IngestionEvent& a, const IngestionEvent& b) { return timestamp_of(a) < timestamp_of(b); });
    return scenario;
}

void report_and_check_latency(const LiveConsumer& consumer, const char* label) {
    std::vector<int64_t> book_ns;
    std::vector<int64_t> detector_ns;
    book_ns.reserve(consumer.latency_samples().size());
    detector_ns.reserve(consumer.latency_samples().size());
    for (const auto& sample : consumer.latency_samples()) {
        book_ns.push_back(sample.book_apply_ns);
        detector_ns.push_back(sample.detectors_ns);
    }
    std::sort(book_ns.begin(), book_ns.end());
    std::sort(detector_ns.begin(), detector_ns.end());

    std::cerr << "[" << label << "] book_apply_ns:  mean=" << mean_ns(book_ns) << " p50=" << percentile_ns(book_ns, 0.50)
              << " p99=" << percentile_ns(book_ns, 0.99) << " max=" << (book_ns.empty() ? 0 : book_ns.back()) << "\n";
    std::cerr << "[" << label << "] detectors_ns:   mean=" << mean_ns(detector_ns)
              << " p50=" << percentile_ns(detector_ns, 0.50) << " p99=" << percentile_ns(detector_ns, 0.99)
              << " max=" << (detector_ns.empty() ? 0 : detector_ns.back()) << "\n";

    // Latency budget -- concrete and documented, not vibes: rule-based
    // detection across all 5 detectors must not materially slow book
    // updates. "Materially" defined as: mean detector-evaluation time
    // stays within 20x mean book-apply time (plus a small additive floor so
    // two very-fast-but-noisy measurements near zero can't trip a pure
    // ratio check), and both stay comfortably under a millisecond even
    // under sanitizer instrumentation. See cpp/pipeline/README.md for the
    // actual measured numbers from a real run of this test, across all
    // three build configs.
    EXPECT_LT(mean_ns(detector_ns), mean_ns(book_ns) * 20.0 + 1000.0);
    EXPECT_LT(mean_ns(detector_ns), 1'000'000.0);              // 1ms mean
    EXPECT_LT(percentile_ns(detector_ns, 0.99), 5'000'000.0);  // 5ms p99
}

}  // namespace

// The primary Phase 6 sustained-load test: the producer paces itself
// against the consumer's progress (yielding while the queue is over half
// full), mirroring Phase 3's SustainedLoadWithConsumerKeepingUpDropsNothing
// — the point here is to get the real book-update-plus-five-detector
// workload to actually process close to the full 100k+-event stream, so
// the latency numbers below are a genuine sustained-load measurement, not
// a handful of samples from whatever fraction of a burst the consumer
// managed to drain before the rest got dropped (see the second test in
// this file for that scenario, which is deliberately the opposite case).
TEST(LiveConsumerSustainedLoad, ConsumerKeepingUpProcessesNearlyEveryEventAndReportsLatency) {
    Scenario scenario = build_scenario(777);
    const size_t total_events = scenario.events.size();
    ASSERT_GE(total_events, 100'000u)
        << "scenario produced only " << total_events << " events -- below Phase 3's 100k-event standard";

    AccountRegistry accounts;
    for (const auto& account : scenario.simulation.accounts) accounts.add(to_entity(account));

    LivePipeline live_pipeline(make_all_five_detectors(scenario.simulation), std::move(accounts));
    CollectingAlertSink alert_sink;
    SpscRingBuffer<IngestionEvent> queue(4096);
    LiveConsumer consumer(queue, live_pipeline, &alert_sink);

    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        for (const auto& event : scenario.events) {
            while (queue.size_approx() > queue.capacity() / 2) {
                std::this_thread::yield();
            }
            queue.push(event);
        }
        producer_done.store(true, std::memory_order_release);
    });
    std::thread consumer_thread([&] { consumer.run(producer_done); });

    producer.join();
    consumer_thread.join();

    EXPECT_EQ(consumer.events_processed() + queue.dropped_count(), total_events);

    std::cerr << "[ConsumerKeepingUp] total_events=" << total_events << " processed=" << consumer.events_processed()
              << " dropped=" << queue.dropped_count() << " skipped_inconsistent=" << consumer.events_skipped_inconsistent()
              << " alerts=" << alert_sink.alerts().size() << "\n";

    // Pacing should get the real consumer workload through the large
    // majority of the stream -- a small allowance (not a strict 0) for
    // scheduling jitter around thread start/stop, matching the spirit of
    // Phase 3's own "keeping up" test without assuming an OS scheduler
    // guarantee this test doesn't control.
    EXPECT_GE(consumer.events_processed(), static_cast<uint64_t>(total_events * 0.99));

    EXPECT_GT(alert_sink.alerts().size(), 0u)
        << "expected at least some alerts from the injected abuse patterns to survive end-to-end";

    report_and_check_latency(consumer, "ConsumerKeepingUp");
}

// The opposite, deliberately adversarial case: the producer floods the
// queue as fast as raw struct copies allow, with no pacing, so the real
// book-plus-detector consumer falls far behind and drop-oldest fires
// heavily. This is not the test the latency numbers should be read from —
// it exists to prove the drop-induced-inconsistency handling
// (LivePipeline::process()'s std::invalid_argument catch) is genuinely
// exercised and safe under real, severe backpressure with a real
// multi-detector workload, not just in the hand-constructed single-event
// case already covered in live_pipeline_test.cpp.
TEST(LiveConsumerSustainedLoad, UnpacedBurstUnderSevereBackpressureNeverCrashesAndAccountingHolds) {
    Scenario scenario = build_scenario(778);
    const size_t total_events = scenario.events.size();
    ASSERT_GE(total_events, 100'000u);

    AccountRegistry accounts;
    for (const auto& account : scenario.simulation.accounts) accounts.add(to_entity(account));

    LivePipeline live_pipeline(make_all_five_detectors(scenario.simulation), std::move(accounts));
    CollectingAlertSink alert_sink;
    SpscRingBuffer<IngestionEvent> queue(4096);
    LiveConsumer consumer(queue, live_pipeline, &alert_sink);

    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        for (const auto& event : scenario.events) queue.push(event);
        producer_done.store(true, std::memory_order_release);
    });
    std::thread consumer_thread([&] { consumer.run(producer_done); });

    producer.join();
    consumer_thread.join();

    EXPECT_EQ(consumer.events_processed() + queue.dropped_count(), total_events);
    EXPECT_GT(queue.dropped_count(), 0u) << "expected this unpaced burst to actually exercise drop-oldest";

    std::cerr << "[UnpacedBurst] total_events=" << total_events << " processed=" << consumer.events_processed()
              << " dropped=" << queue.dropped_count() << " skipped_inconsistent=" << consumer.events_skipped_inconsistent()
              << " alerts=" << alert_sink.alerts().size() << "\n";
}
