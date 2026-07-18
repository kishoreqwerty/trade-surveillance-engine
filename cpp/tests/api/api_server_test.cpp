#include "api_server.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <httplib.h>

#include "account_registry.hpp"
#include "db_test_helpers.hpp"
#include "live_book_registry.hpp"
#include "live_pipeline.hpp"
#include "test_api_server.hpp"
#include "wash_trade_detector.hpp"

using tse::api::LiveBookRegistry;
using tse::api::testutil::TestApiServer;
using tse::db::testutil::connect_or_skip;
using tse::detectors::Alert;
using tse::fix::Order;
using tse::fix::OrderStatus;
using tse::fix::OrderType;
using tse::fix::Side;
using tse::ingestion::IngestionEvent;
using tse::ingestion::SpscRingBuffer;
using tse::pipeline::LivePipeline;

namespace {

Alert make_alert(const std::string& detector_name, const std::vector<std::string>& account_ids,
                  int64_t window_start_ns) {
    Alert alert;
    alert.detector_name = detector_name;
    alert.score = 0.75;
    alert.instrument_id = "ACME";
    alert.account_ids = account_ids;
    alert.order_ids = {"O1"};
    alert.window_start_ns = window_start_ns;
    alert.window_end_ns = window_start_ns + 1000;
    alert.evidence = "test evidence";
    return alert;
}

Order make_new(const std::string& id, const std::string& account, const std::string& instrument, int64_t ts) {
    Order order;
    order.order_id = id;
    order.orig_order_id = id;
    order.account_id = account;
    order.instrument_id = instrument;
    order.side = Side::kBuy;
    order.price = 100.0;
    order.qty = 500;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = ts;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    return order;
}

constexpr int kTestPort = 18400;

}  // namespace

class ApiServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = connect_or_skip();
        if (!store_) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";
        server_ = std::make_unique<TestApiServer>(kTestPort, store_.get(), /*book_registry=*/nullptr);
        client_ = std::make_unique<httplib::Client>("http://127.0.0.1:" + std::to_string(kTestPort));
    }

    std::unique_ptr<tse::db::AlertStore> store_;
    std::unique_ptr<TestApiServer> server_;
    std::unique_ptr<httplib::Client> client_;
};

TEST_F(ApiServerTest, HealthEndpointReturns200) {
    auto res = client_->Get("/api/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
}

TEST_F(ApiServerTest, ListRecentAlertsWithNoFilterReturnsInsertedAlerts) {
    store_->insert_alert(make_alert("WashTradeDetector", {"ACC-1"}, 1'000'000'000LL));
    store_->insert_alert(make_alert("SpoofingLayeringDetector", {"ACC-2"}, 2'000'000'000LL));

    auto res = client_->Get("/api/alerts");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto parsed = crow::json::load(res->body);
    ASSERT_TRUE(parsed);
    EXPECT_GE(parsed["alerts"].size(), 2u);
}

TEST_F(ApiServerTest, FilterByAccountReturnsOnlyMatchingAlerts) {
    store_->insert_alert(make_alert("WashTradeDetector", {"ACC-X1"}, 1'000'000'000LL));
    store_->insert_alert(make_alert("SpoofingLayeringDetector", {"ACC-X2"}, 2'000'000'000LL));

    auto res = client_->Get("/api/alerts?account_id=ACC-X1");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto parsed = crow::json::load(res->body);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed["alerts"].size(), 1u);
    EXPECT_EQ(std::string(parsed["alerts"][0]["detector_name"]), "WashTradeDetector");
}

TEST_F(ApiServerTest, FilterByDetectorNameReturnsOnlyMatchingAlerts) {
    store_->insert_alert(make_alert("MarkingTheCloseDetector", {"ACC-Y1"}, 1'000'000'000LL));
    store_->insert_alert(make_alert("FrontRunningDetector", {"ACC-Y2"}, 2'000'000'000LL));

    auto res = client_->Get("/api/alerts?detector_name=MarkingTheCloseDetector");
    ASSERT_TRUE(res);
    auto parsed = crow::json::load(res->body);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed["alerts"].size(), 1u);
    EXPECT_EQ(std::string(parsed["alerts"][0]["detector_name"]), "MarkingTheCloseDetector");
}

TEST_F(ApiServerTest, FilterByTimeRangeReturnsOnlyMatchingAlerts) {
    store_->insert_alert(make_alert("WashTradeDetector", {"ACC-Z1"}, 5'000'000'000LL));
    store_->insert_alert(make_alert("WashTradeDetector", {"ACC-Z2"}, 50'000'000'000LL));

    auto res = client_->Get("/api/alerts?start_ns=1000000000&end_ns=10000000000");
    ASSERT_TRUE(res);
    auto parsed = crow::json::load(res->body);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed["alerts"].size(), 1u);
    EXPECT_EQ(std::string(parsed["alerts"][0]["account_ids"][0]), "ACC-Z1");
}

TEST_F(ApiServerTest, GetSingleAlertByIdReturnsIt) {
    int64_t alert_id = store_->insert_alert(make_alert("WashTradeDetector", {"ACC-1"}, 1'000'000'000LL));

    auto res = client_->Get("/api/alerts/" + std::to_string(alert_id));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto parsed = crow::json::load(res->body);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed["alert_id"].i(), alert_id);
}

TEST_F(ApiServerTest, GetSingleAlertWithUnknownIdReturns404) {
    auto res = client_->Get("/api/alerts/999999999");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(ApiServerTest, GetSingleAlertWithNonNumericIdReturns400) {
    auto res = client_->Get("/api/alerts/not-a-number");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(ApiServerTest, PatchStatusUpdatesStatus) {
    int64_t alert_id = store_->insert_alert(make_alert("WashTradeDetector", {"ACC-1"}, 1'000'000'000LL));

    auto res = client_->Patch("/api/alerts/" + std::to_string(alert_id) + "/status",
                               R"({"status":"UNDER_REVIEW"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto parsed = crow::json::load(res->body);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(std::string(parsed["status"]), "UNDER_REVIEW");

    EXPECT_EQ(store_->get_alert(alert_id)->status, "UNDER_REVIEW");
}

TEST_F(ApiServerTest, PatchStatusWithInvalidValueReturns400) {
    int64_t alert_id = store_->insert_alert(make_alert("WashTradeDetector", {"ACC-1"}, 1'000'000'000LL));

    auto res = client_->Patch("/api/alerts/" + std::to_string(alert_id) + "/status",
                               R"({"status":"NOT_REAL"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(store_->get_alert(alert_id)->status, "OPEN") << "the rejected update must not have applied";
}

TEST_F(ApiServerTest, PatchStatusWithUnknownIdReturns400) {
    auto res =
        client_->Patch("/api/alerts/999999999/status", R"({"status":"CLOSED"})", "application/json");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(ApiServerTest, OrderBookSnapshotWithoutRegistryReturns503) {
    // server_ was constructed with book_registry=nullptr in SetUp().
    auto res = client_->Get("/api/orderbook/ACME/snapshot");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 503);
}

// A second fixture, separate from ApiServerTest, specifically for the
// order-book endpoint -- needs a real LivePipeline/LiveBookRegistry wired
// in, which the base fixture's nullptr-registry tests above deliberately
// don't have.
class ApiServerOrderBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = connect_or_skip();
        if (!store_) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

        tse::detectors::AccountRegistry accounts;
        std::vector<std::unique_ptr<tse::detectors::IDetector>> detectors;
        detectors.push_back(std::make_unique<tse::detectors::WashTradeDetector>());
        pipeline_ = std::make_unique<LivePipeline>(std::move(detectors), std::move(accounts));
        queue_ = std::make_unique<SpscRingBuffer<IngestionEvent>>(64);
        registry_ = std::make_unique<LiveBookRegistry>(*queue_, *pipeline_, /*alert_sink=*/nullptr);

        consumer_thread_ = std::thread([this] { registry_->run(producer_done_); });
        server_ = std::make_unique<TestApiServer>(kTestPort + 1, store_.get(), registry_.get());
        client_ = std::make_unique<httplib::Client>("http://127.0.0.1:" + std::to_string(kTestPort + 1));
    }

    void TearDown() override {
        if (!store_) return;  // SetUp GTEST_SKIP'd before constructing any of this
        producer_done_.store(true, std::memory_order_release);
        consumer_thread_.join();
    }

    void push_and_wait(const IngestionEvent& event) {
        uint64_t before = registry_->events_processed();
        queue_->push(event);
        for (int i = 0; i < 200 && registry_->events_processed() == before; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::unique_ptr<tse::db::AlertStore> store_;
    std::unique_ptr<LivePipeline> pipeline_;
    std::unique_ptr<SpscRingBuffer<IngestionEvent>> queue_;
    std::unique_ptr<LiveBookRegistry> registry_;
    std::atomic<bool> producer_done_{false};
    std::thread consumer_thread_;
    std::unique_ptr<TestApiServer> server_;
    std::unique_ptr<httplib::Client> client_;
};

TEST_F(ApiServerOrderBookTest, UnknownInstrumentReturns404) {
    auto res = client_->Get("/api/orderbook/NEVER-TRADED/snapshot");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 404);
}

TEST_F(ApiServerOrderBookTest, ReturnsRealBookStateAfterAnOrderIsProcessed) {
    push_and_wait(IngestionEvent{make_new("O1", "ACC-1", "ACME", 1000)});

    auto res = client_->Get("/api/orderbook/ACME/snapshot");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    auto parsed = crow::json::load(res->body);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(std::string(parsed["instrument_id"]), "ACME");
    EXPECT_EQ(parsed["sequence"].i(), 1);
    ASSERT_EQ(parsed["bids"].size(), 1u);
    EXPECT_DOUBLE_EQ(parsed["bids"][0]["price"].d(), 100.0);
    EXPECT_EQ(parsed["bids"][0]["total_qty"].i(), 500);
}
