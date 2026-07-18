#include "alert_store.hpp"

#include <gtest/gtest.h>

#include <pqxx/pqxx>

#include "db_config.hpp"
#include "db_test_helpers.hpp"

using tse::db::AlertStore;
using tse::db::StoredAlert;
using tse::db::testutil::connect_or_skip;
using tse::detectors::Alert;
using tse::fix::Execution;
using tse::fix::Order;
using tse::fix::OrderStatus;
using tse::fix::OrderType;
using tse::fix::Side;

namespace {

Order make_order() {
    Order order;
    order.order_id = "O1";
    order.orig_order_id = "O1";
    order.account_id = "ACC-1";
    order.instrument_id = "ACME";
    order.side = Side::kBuy;
    order.price = 101.5;
    order.qty = 250;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = 1'700'000'000'000'000'000LL;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    return order;
}

Execution make_execution() {
    Execution execution;
    execution.trade_id = "T1";
    execution.order_id = "O1";
    execution.account_id = "ACC-1";
    execution.counterparty_account_id = "ACC-2";
    execution.instrument_id = "ACME";
    execution.side = Side::kBuy;
    execution.price = 101.5;
    execution.qty = 250;
    execution.timestamp_ns = 1'700'000'000'100'000'000LL;
    execution.venue = "SIM";
    return execution;
}

Alert make_alert() {
    Alert alert;
    alert.detector_name = "WashTradeDetector";
    alert.score = 1.0;
    alert.instrument_id = "ACME";
    // Deliberately includes a comma and a double-quote -- exercises
    // to_pg_text_array()/from_pg_text_array()'s escaping, not just the
    // happy path of plain alphanumeric IDs.
    alert.account_ids = {"ACC-1", "ACC,2", "ACC\"3"};
    alert.order_ids = {"O1", "O2"};
    alert.window_start_ns = 1'700'000'000'000'000'000LL;
    alert.window_end_ns = 1'700'000'000'100'000'000LL;
    alert.evidence = "test evidence";
    alert.model_version = "isoforest-abc123";
    alert.book_snapshot_sequence = 42;
    return alert;
}

// A second, raw connection deliberately separate from AlertStore's own --
// AlertStore has no reason to expose a general "count arbitrary catalog
// rows" method, but this test needs to look at Postgres's own bookkeeping
// (timescaledb_information.hypertables, pg_indexes) to prove idempotency,
// not just that apply_schema() didn't throw.
int64_t count_hypertables() {
    pqxx::connection conn(tse::db::DbConfig{}.connection_string());
    pqxx::work txn(conn);
    pqxx::row r = txn.exec1("SELECT count(*) FROM timescaledb_information.hypertables");
    return r[0].as<int64_t>();
}

int64_t count_indexes() {
    pqxx::connection conn(tse::db::DbConfig{}.connection_string());
    pqxx::work txn(conn);
    pqxx::row r = txn.exec1("SELECT count(*) FROM pg_indexes WHERE schemaname = 'public'");
    return r[0].as<int64_t>();
}

}  // namespace

// "No error" alone doesn't prove idempotency -- a schema that dropped and
// recreated objects on every call, or silently accumulated duplicate
// indexes, would also pass an EXPECT_NO_THROW-only check. This asserts the
// actual catalog state (hypertable count, index count) is bit-for-bit
// unchanged across three consecutive applications, not just that none of
// them threw.
TEST(AlertStore, ApplySchemaIsIdempotent) {
    auto store = connect_or_skip();  // connect_or_skip() itself already calls apply_schema() once
    if (!store) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

    const int64_t hypertables_after_first = count_hypertables();
    const int64_t indexes_after_first = count_indexes();
    ASSERT_EQ(hypertables_after_first, 3) << "orders, trades, alerts should all be registered hypertables";

    EXPECT_NO_THROW(store->apply_schema());
    EXPECT_EQ(count_hypertables(), hypertables_after_first);
    EXPECT_EQ(count_indexes(), indexes_after_first);

    EXPECT_NO_THROW(store->apply_schema());
    EXPECT_EQ(count_hypertables(), hypertables_after_first);
    EXPECT_EQ(count_indexes(), indexes_after_first);
}

TEST(AlertStore, InsertOrderThenQueryRoundTripsExactly) {
    auto store = connect_or_skip();
    if (!store) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

    EXPECT_NO_THROW(store->insert_order(make_order()));
}

TEST(AlertStore, InsertExecutionSucceeds) {
    auto store = connect_or_skip();
    if (!store) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

    EXPECT_NO_THROW(store->insert_execution(make_execution()));
}

TEST(AlertStore, InsertAlertAssignsIdAndRoundTripsEveryField) {
    auto store = connect_or_skip();
    if (!store) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

    const Alert original = make_alert();
    const int64_t alert_id = store->insert_alert(original);
    EXPECT_GT(alert_id, 0);

    std::vector<StoredAlert> found = store->query_alerts_by_detector("WashTradeDetector");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].alert_id, alert_id);
    const Alert& round_tripped = found[0].alert;

    EXPECT_EQ(round_tripped.detector_name, original.detector_name);
    EXPECT_DOUBLE_EQ(round_tripped.score, original.score);
    EXPECT_EQ(round_tripped.instrument_id, original.instrument_id);
    EXPECT_EQ(round_tripped.account_ids, original.account_ids);
    EXPECT_EQ(round_tripped.order_ids, original.order_ids);
    EXPECT_EQ(round_tripped.window_start_ns, original.window_start_ns);
    EXPECT_EQ(round_tripped.window_end_ns, original.window_end_ns);
    EXPECT_EQ(round_tripped.evidence, original.evidence);
    ASSERT_TRUE(round_tripped.model_version.has_value());
    EXPECT_EQ(*round_tripped.model_version, *original.model_version);
    ASSERT_TRUE(round_tripped.book_snapshot_sequence.has_value());
    EXPECT_EQ(*round_tripped.book_snapshot_sequence, *original.book_snapshot_sequence);
    EXPECT_EQ(found[0].status, "OPEN") << "schema.sql's column default -- no detector or insert_alert() caller "
                                           "ever sets status explicitly";
}

// A deterministic-rule detector (e.g. WashTradeDetector) never sets
// model_version, and a hand-constructed Alert in a test may not set
// book_snapshot_sequence either -- both must land as real SQL NULL, not a
// sentinel value, and round-trip back as an unset std::optional, not an
// empty string or zero.
TEST(AlertStore, OptionalFieldsRoundTripAsNullNotSentinelValues) {
    auto store = connect_or_skip();
    if (!store) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

    Alert alert = make_alert();
    alert.detector_name = "FrontRunningDetector";
    alert.model_version.reset();
    alert.book_snapshot_sequence.reset();
    store->insert_alert(alert);

    std::vector<StoredAlert> found = store->query_alerts_by_detector("FrontRunningDetector");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_FALSE(found[0].alert.model_version.has_value());
    EXPECT_FALSE(found[0].alert.book_snapshot_sequence.has_value());
}

TEST(AlertStore, GetAlertReturnsTheMatchingAlert) {
    auto store = connect_or_skip();
    if (!store) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

    const int64_t alert_id = store->insert_alert(make_alert());

    std::optional<StoredAlert> found = store->get_alert(alert_id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->alert_id, alert_id);
    EXPECT_EQ(found->alert.detector_name, "WashTradeDetector");
}

TEST(AlertStore, GetAlertWithUnknownIdReturnsNullopt) {
    auto store = connect_or_skip();
    if (!store) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

    EXPECT_FALSE(store->get_alert(999999999).has_value());
}

TEST(AlertStore, ListRecentAlertsReturnsMostRecentFirstUpToLimit) {
    auto store = connect_or_skip();
    if (!store) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

    for (int i = 0; i < 5; ++i) {
        Alert alert = make_alert();
        alert.window_start_ns = 1'000'000'000LL * (i + 1);
        store->insert_alert(alert);
    }

    std::vector<StoredAlert> recent = store->list_recent_alerts(3);
    ASSERT_EQ(recent.size(), 3u);
    // Most recent (largest window_start_ns) first.
    EXPECT_EQ(recent[0].alert.window_start_ns, 5'000'000'000LL);
    EXPECT_EQ(recent[1].alert.window_start_ns, 4'000'000'000LL);
    EXPECT_EQ(recent[2].alert.window_start_ns, 3'000'000'000LL);
}

TEST(AlertStore, UpdateAlertStatusChangesStatusAndOnlyThatAlert) {
    auto store = connect_or_skip();
    if (!store) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

    const int64_t target_id = store->insert_alert(make_alert());
    const int64_t other_id = store->insert_alert(make_alert());

    store->update_alert_status(target_id, "UNDER_REVIEW");

    EXPECT_EQ(store->get_alert(target_id)->status, "UNDER_REVIEW");
    EXPECT_EQ(store->get_alert(other_id)->status, "OPEN") << "updating one alert must not affect another";
}

TEST(AlertStore, UpdateAlertStatusWithInvalidValueThrows) {
    auto store = connect_or_skip();
    if (!store) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

    const int64_t alert_id = store->insert_alert(make_alert());
    EXPECT_THROW(store->update_alert_status(alert_id, "NOT_A_REAL_STATUS"), std::exception);
    // The rejected update must not have partially applied.
    EXPECT_EQ(store->get_alert(alert_id)->status, "OPEN");
}

TEST(AlertStore, UpdateAlertStatusWithUnknownIdThrows) {
    auto store = connect_or_skip();
    if (!store) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

    EXPECT_THROW(store->update_alert_status(999999999, "CLOSED"), std::runtime_error);
}
