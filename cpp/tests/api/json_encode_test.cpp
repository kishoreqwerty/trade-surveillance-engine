#include "json_encode.hpp"

#include <gtest/gtest.h>

using tse::db::StoredAlert;
using tse::detectors::Alert;
using tse::orderbook::DepthSnapshot;
using tse::orderbook::PriceLevel;
using tse::orderbook::RestingOrderSummary;

namespace {
StoredAlert make_stored_alert() {
    StoredAlert stored;
    stored.alert_id = 7;
    stored.status = "UNDER_REVIEW";
    stored.alert.detector_name = "SpoofingLayeringDetector";
    stored.alert.score = 0.85;
    stored.alert.instrument_id = "ACME";
    stored.alert.account_ids = {"ACC-1", "ACC-2"};
    stored.alert.order_ids = {"O1"};
    stored.alert.window_start_ns = 1000;
    stored.alert.window_end_ns = 2000;
    stored.alert.evidence = "test evidence";
    return stored;
}
}  // namespace

TEST(JsonEncode, EncodeAlertWithNullOptionalFieldsOmitsThemAsJsonNull) {
    StoredAlert stored = make_stored_alert();
    crow::json::wvalue encoded = tse::api::encode_alert(stored);
    crow::json::rvalue parsed = crow::json::load(encoded.dump());

    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed["alert_id"].i(), 7);
    EXPECT_EQ(std::string(parsed["status"]), "UNDER_REVIEW");
    EXPECT_EQ(std::string(parsed["detector_name"]), "SpoofingLayeringDetector");
    EXPECT_DOUBLE_EQ(parsed["score"].d(), 0.85);
    EXPECT_EQ(std::string(parsed["instrument_id"]), "ACME");
    ASSERT_EQ(parsed["account_ids"].size(), 2u);
    EXPECT_EQ(std::string(parsed["account_ids"][0]), "ACC-1");
    EXPECT_EQ(std::string(parsed["account_ids"][1]), "ACC-2");
    ASSERT_EQ(parsed["order_ids"].size(), 1u);
    EXPECT_EQ(std::string(parsed["order_ids"][0]), "O1");
    EXPECT_EQ(parsed["window_start_ns"].i(), 1000);
    EXPECT_EQ(parsed["window_end_ns"].i(), 2000);
    EXPECT_EQ(std::string(parsed["evidence"]), "test evidence");
    EXPECT_EQ(parsed["model_version"].t(), crow::json::type::Null);
    EXPECT_EQ(parsed["book_snapshot_sequence"].t(), crow::json::type::Null);
}

TEST(JsonEncode, EncodeAlertWithSetOptionalFieldsIncludesTheirValues) {
    StoredAlert stored = make_stored_alert();
    stored.alert.model_version = "isoforest-abc123";
    stored.alert.book_snapshot_sequence = 42;

    crow::json::wvalue encoded = tse::api::encode_alert(stored);
    crow::json::rvalue parsed = crow::json::load(encoded.dump());

    ASSERT_TRUE(parsed);
    EXPECT_EQ(std::string(parsed["model_version"]), "isoforest-abc123");
    EXPECT_EQ(parsed["book_snapshot_sequence"].i(), 42);
}

TEST(JsonEncode, EncodeAlertsWrapsListUnderAlertsKey) {
    std::vector<StoredAlert> alerts = {make_stored_alert(), make_stored_alert()};
    alerts[1].alert_id = 8;

    crow::json::wvalue encoded = tse::api::encode_alerts(alerts);
    crow::json::rvalue parsed = crow::json::load(encoded.dump());

    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed["alerts"].size(), 2u);
    EXPECT_EQ(parsed["alerts"][0]["alert_id"].i(), 7);
    EXPECT_EQ(parsed["alerts"][1]["alert_id"].i(), 8);
}

TEST(JsonEncode, EncodeDepthSnapshotIncludesBidsAsksAndRestingOrderDetail) {
    DepthSnapshot snapshot;
    snapshot.instrument_id = "ACME";
    snapshot.sequence = 5;
    snapshot.last_event_timestamp_ns = 12345;
    snapshot.bids = {PriceLevel{100.0, 300, {RestingOrderSummary{"O1", "ACC-1", 200}, RestingOrderSummary{"O2", "ACC-2", 100}}}};
    snapshot.asks = {PriceLevel{101.0, 150, {RestingOrderSummary{"O3", "ACC-3", 150}}}};

    crow::json::wvalue encoded = tse::api::encode_depth_snapshot(snapshot);
    crow::json::rvalue parsed = crow::json::load(encoded.dump());

    ASSERT_TRUE(parsed);
    EXPECT_EQ(std::string(parsed["instrument_id"]), "ACME");
    EXPECT_EQ(parsed["sequence"].i(), 5);
    EXPECT_EQ(parsed["last_event_timestamp_ns"].i(), 12345);
    ASSERT_EQ(parsed["bids"].size(), 1u);
    EXPECT_DOUBLE_EQ(parsed["bids"][0]["price"].d(), 100.0);
    EXPECT_EQ(parsed["bids"][0]["total_qty"].i(), 300);
    ASSERT_EQ(parsed["bids"][0]["orders"].size(), 2u);
    EXPECT_EQ(std::string(parsed["bids"][0]["orders"][0]["order_id"]), "O1");
    EXPECT_EQ(parsed["bids"][0]["orders"][0]["qty"].i(), 200);
    ASSERT_EQ(parsed["asks"].size(), 1u);
    EXPECT_DOUBLE_EQ(parsed["asks"][0]["price"].d(), 101.0);
}

TEST(JsonEncode, EncodeDepthSnapshotWithNoLevelsProducesEmptyArrays) {
    DepthSnapshot snapshot;
    snapshot.instrument_id = "GLBX";

    crow::json::wvalue encoded = tse::api::encode_depth_snapshot(snapshot);
    crow::json::rvalue parsed = crow::json::load(encoded.dump());

    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed["bids"].size(), 0u);
    EXPECT_EQ(parsed["asks"].size(), 0u);
}
