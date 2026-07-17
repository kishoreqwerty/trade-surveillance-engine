#include <gtest/gtest.h>

#include <quickfix/Session.h>

#include <atomic>
#include <thread>
#include <unordered_map>

#include "alert_sink.hpp"
#include "fix_session_test_fixture.hpp"
#include "front_running_detector.hpp"
#include "live_consumer.hpp"
#include "live_pipeline.hpp"
#include "marking_the_close_detector.hpp"
#include "message_translator.hpp"
#include "pipeline_event_sink.hpp"
#include "simulator.hpp"
#include "spoofing_layering_detector.hpp"
#include "statistical_baseline_detector.hpp"
#include "wash_trade_detector.hpp"

using tse::fix::testutil::LiveSessionFixture;

namespace {

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
    out.orig_order_id = order.order_id;
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

tse::detectors::Entity to_entity(const tse::simulator::Account& account) {
    tse::detectors::Entity entity;
    entity.account_id = account.account_id;
    entity.beneficial_owner_id = account.beneficial_owner_id;
    entity.entity_type = tse::simulator::to_string(account.entity_type);
    entity.linked_account_ids = account.linked_account_ids;
    return entity;
}

std::vector<std::unique_ptr<tse::detectors::IDetector>> make_all_five_detectors(
    const tse::simulator::SimulationOutput& simulation) {
    std::vector<std::unique_ptr<tse::detectors::IDetector>> result;
    result.push_back(std::make_unique<tse::detectors::WashTradeDetector>());
    result.push_back(std::make_unique<tse::detectors::SpoofingLayeringDetector>());
    tse::detectors::MarkingTheCloseConfig mtc_config;
    for (const auto& instrument : simulation.instruments) {
        mtc_config.close_time_ns_by_instrument[instrument.instrument_id] = instrument.session_close_ns;
    }
    result.push_back(std::make_unique<tse::detectors::MarkingTheCloseDetector>(mtc_config));
    result.push_back(std::make_unique<tse::detectors::FrontRunningDetector>());
    result.push_back(std::make_unique<tse::detectors::StatisticalBaselineDetector>());
    return result;
}

}  // namespace

// The genuine "FIX flow" leg of Phase 6's wiring, as opposed to
// live_consumer_sustained_load_test.cpp's much larger (100k+ event) but
// FIX-wire-bypassing ring-buffer/book/detectors test. This one drives real
// synthetic messages over a real acceptor+initiator TCP session (the exact
// Phase 2 machinery simulator_end_to_end_test.cpp already proves), with the
// acceptor side's SurveillanceFixApplication wired via set_event_sink()
// (new this phase) into a real SPSC ring buffer, drained by a real
// LiveConsumer into a real LivePipeline on a second thread running
// concurrently with the FIX session's own message flow.
//
// Deliberately a few thousand messages, not 100k+: driving that many
// individual messages through real per-message TCP+QuickFIX-session
// round trips under TSan would take a very long time without adding
// meaningful proof beyond what this test and the sustained-load test
// separately already cover. QuickFIX's own session/transport thread-safety
// was already proven at scale in Phase 2; what's new and actually needs
// proving here is specifically the FIX-callback-thread -> ring-buffer
// handoff (the IEventSink hook this phase adds) -- a moderate, genuinely
// concurrent flow is enough to exercise that new boundary under a
// sanitizer. The ring-buffer -> book -> detectors leg gets its 100k+-event
// proof separately, without needing to pay for real socket I/O to get it.
TEST_F(LiveSessionFixture, DrivesSimulatedFixFlowThroughTheRingBufferIntoTheLivePipeline) {
    ASSERT_TRUE(logon());

    tse::simulator::SimulatorConfig config;
    config.random_seed = 42424;
    config.session_duration_ns = 120'000'000'000;  // 2 synthetic minutes
    config.baseline_orders_per_second = 20.0;
    config.num_equity_instruments = 2;
    config.num_fx_instruments = 1;
    config.num_fixed_income_instruments = 0;
    config.num_independent_accounts = 10;
    config.num_linked_account_pairs = 2;
    config.wash_trade = {2, 0.7};

    tse::simulator::SimulationOutput simulation = tse::simulator::generate_simulation(config);
    ASSERT_FALSE(simulation.orders.empty());
    ASSERT_FALSE(simulation.executions.empty());

    tse::detectors::AccountRegistry accounts;
    for (const auto& account : simulation.accounts) accounts.add(to_entity(account));

    tse::pipeline::LivePipeline live_pipeline(make_all_five_detectors(simulation), std::move(accounts));
    tse::pipeline::CollectingAlertSink alert_sink;
    tse::ingestion::SpscRingBuffer<tse::ingestion::IngestionEvent> queue(4096);
    tse::pipeline::LiveConsumer consumer(queue, live_pipeline, &alert_sink);
    tse::pipeline::PipelineEventSink event_sink(queue, /*kafka_producer=*/nullptr);

    acceptor_app_.set_event_sink(&event_sink);

    std::atomic<bool> producer_done{false};
    // Started before any message is sent, and joined only after every
    // message has been confirmed received -- the consumer genuinely runs
    // concurrently with the FIX session delivering messages, not
    // sequentially before/after it.
    std::thread consumer_thread([&] { consumer.run(producer_done); });

    std::unordered_map<std::string, tse::fix::Side> side_by_order_id;
    for (const auto& order : simulation.orders) {
        side_by_order_id[order.order_id] =
            order.side == tse::simulator::Side::kBuy ? tse::fix::Side::kBuy : tse::fix::Side::kSell;
    }

    size_t expected_orders = 0;
    for (const auto& order : simulation.orders) {
        tse::fix::Order fix_order = to_fix_order(order);
        if (order.status == tse::simulator::OrderStatus::kNew) {
            auto message = tse::fix::to_new_order_single(fix_order);
            ASSERT_TRUE(FIX::Session::sendToTarget(message, initiator_session_id_));
            ++expected_orders;
        } else if (order.status == tse::simulator::OrderStatus::kCancelled) {
            auto message = tse::fix::to_order_cancel_request(fix_order);
            ASSERT_TRUE(FIX::Session::sendToTarget(message, initiator_session_id_));
            ++expected_orders;
        }
    }
    for (const auto& execution : simulation.executions) {
        tse::fix::Side side = side_by_order_id.at(execution.order_id);
        auto message = tse::fix::to_execution_report(to_fix_execution(execution, side));
        ASSERT_TRUE(FIX::Session::sendToTarget(message, initiator_session_id_));
    }
    const size_t total_sent = expected_orders + simulation.executions.size();
    ASSERT_GT(total_sent, 1000u) << "scenario too small to be a genuine sustained FIX-wire flow";

    ASSERT_TRUE(acceptor_app_.wait_for_event_count(std::chrono::seconds(60), expected_orders,
                                                    simulation.executions.size()));

    producer_done.store(true, std::memory_order_release);
    consumer_thread.join();

    EXPECT_EQ(consumer.events_processed() + queue.dropped_count(), total_sent);
    EXPECT_EQ(queue.dropped_count(), 0u)
        << "a ring buffer sized for this scenario's volume shouldn't need to drop anything";
    EXPECT_GT(alert_sink.alerts().size(), 0u) << "expected the injected wash-trade pattern to survive end-to-end, "
                                                  "through real FIX wire encoding, into a live alert";

    // A live book genuinely got built from data that round-tripped through
    // real FIX wire encoding, not from a direct in-memory feed.
    const auto* some_book = live_pipeline.book_for(simulation.instruments.front().instrument_id);
    EXPECT_NE(some_book, nullptr);
}
