#include "db_alert_sink.hpp"

#include <gtest/gtest.h>

#include "front_running_detector.hpp"
#include "live_pipeline.hpp"
#include "statistical_baseline_detector.hpp"
#include "wash_trade_detector.hpp"

#include "db_test_helpers.hpp"

using tse::db::AlertStore;
using tse::db::DbAlertSink;
using tse::db::StoredAlert;
using tse::db::testutil::connect_or_skip;
using tse::detectors::AccountRegistry;
using tse::detectors::DetectorEvent;
using tse::detectors::Entity;
using tse::detectors::IDetector;
using tse::detectors::WashTradeDetector;
using tse::fix::Execution;
using tse::fix::Order;
using tse::fix::OrderStatus;
using tse::fix::OrderType;
using tse::fix::Side;
using tse::pipeline::LivePipeline;
using tse::pipeline::ProcessResult;

namespace {

Order make_new(const std::string& id, const std::string& account, const std::string& instrument, Side side,
               double price, int64_t qty, int64_t ts) {
    Order order;
    order.order_id = id;
    order.orig_order_id = id;
    order.account_id = account;
    order.instrument_id = instrument;
    order.side = side;
    order.price = price;
    order.qty = qty;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = ts;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    return order;
}

Execution make_execution(const std::string& order_id, const std::string& account, const std::string& counterparty,
                          const std::string& instrument, int64_t qty, int64_t ts) {
    Execution execution;
    execution.trade_id = "T-" + order_id;
    execution.order_id = order_id;
    execution.account_id = account;
    execution.counterparty_account_id = counterparty;
    execution.instrument_id = instrument;
    execution.side = Side::kBuy;
    execution.price = 100.0;
    execution.qty = qty;
    execution.timestamp_ns = ts;
    execution.venue = "SIM";
    return execution;
}

}  // namespace

// The build guide's actual Phase 8 "Done when": "alerts generated in Phase
// 6/7 land correctly in TimescaleDB with full evidence." This drives a
// real Alert through the real LivePipeline (Phase 6) -- not a
// hand-constructed Alert struct, the same class/detector/OrderBook wiring
// cpp/tests/pipeline/live_pipeline_test.cpp already proves fires correctly
// -- through a real DbAlertSink into a real database, then reads it back
// via AlertStore's query layer and checks every field of the *original*
// fired Alert survived the round trip, including the book_snapshot_sequence
// this phase added to Alert specifically so persisted evidence could
// reference the exact book state a detector was looking at.
TEST(DbAlertSinkIntegration, RealPipelineAlertLandsInTimescaleDbWithFullEvidence) {
    auto store = connect_or_skip();
    if (!store) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

    AccountRegistry accounts;
    accounts.add(Entity{"ACC-1", "OWNER-A", "client", {}});
    accounts.add(Entity{"ACC-2", "OWNER-A", "client", {}});

    std::vector<std::unique_ptr<IDetector>> detectors;
    detectors.push_back(std::make_unique<WashTradeDetector>());
    LivePipeline pipeline(std::move(detectors), std::move(accounts));

    DbAlertSink sink(store.get());

    // New, then the ExecutionReport that fills it against a related
    // counterparty -- exactly cpp/tests/pipeline/live_pipeline_test.cpp's
    // RealDetectorFiresWhenDrivenThroughTheFullPipeline scenario, the one
    // Phase 6 already proved wires OrderBook/AccountRegistry through
    // correctly. This test's job starts where that one's ends: does the
    // resulting real Alert actually land in TimescaleDB intact.
    pipeline.process(DetectorEvent{make_new("O1", "ACC-1", "ACME", Side::kBuy, 100.00, 500, 999)});
    ProcessResult result = pipeline.process(DetectorEvent{make_execution("O1", "ACC-1", "ACC-2", "ACME", 500, 1000)});

    ASSERT_EQ(result.alerts.size(), 1u);
    const auto& fired = result.alerts[0];
    EXPECT_EQ(fired.detector_name, "WashTradeDetector");
    ASSERT_TRUE(fired.book_snapshot_sequence.has_value())
        << "WashTradeDetector::evaluate() populates book_snapshot_sequence on every fired alert (this phase's change)";

    for (const auto& alert : result.alerts) sink.on_alert(alert);

    std::vector<StoredAlert> found = store->query_alerts_by_detector("WashTradeDetector");
    ASSERT_EQ(found.size(), 1u);
    const auto& persisted = found[0].alert;

    EXPECT_EQ(persisted.detector_name, fired.detector_name);
    EXPECT_DOUBLE_EQ(persisted.score, fired.score);
    EXPECT_EQ(persisted.instrument_id, fired.instrument_id);
    EXPECT_EQ(persisted.account_ids, fired.account_ids);
    EXPECT_EQ(persisted.order_ids, fired.order_ids);
    EXPECT_EQ(persisted.window_start_ns, fired.window_start_ns);
    EXPECT_EQ(persisted.window_end_ns, fired.window_end_ns);
    EXPECT_EQ(persisted.evidence, fired.evidence);
    EXPECT_FALSE(persisted.model_version.has_value())
        << "WashTradeDetector is a deterministic rule, not model-backed -- model_version must stay null";
    ASSERT_TRUE(persisted.book_snapshot_sequence.has_value());
    EXPECT_EQ(*persisted.book_snapshot_sequence, *fired.book_snapshot_sequence);

    // Confirms filter-by-account also sees this real, pipeline-produced
    // alert -- not just the hand-constructed fixtures in
    // alert_store_query_test.cpp.
    std::vector<StoredAlert> by_account = store->query_alerts_by_account("ACC-1");
    EXPECT_EQ(by_account.size(), 1u);
}
