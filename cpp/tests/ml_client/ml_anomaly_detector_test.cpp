#include "ml_anomaly_detector.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "account_registry.hpp"
#include "alert_sink.hpp"
#include "order_book.hpp"
#include "test_http_server.hpp"

using tse::detectors::AccountRegistry;
using tse::detectors::DetectorEvent;
using tse::fix::Execution;
using tse::fix::Order;
using tse::fix::OrderStatus;
using tse::fix::OrderType;
using tse::fix::Side;
using tse::ml_client::MlAnomalyDetector;
using tse::ml_client::MlAnomalyDetectorConfig;
using tse::ml_client::MlScoreClient;
using tse::ml_client::MlScoreClientConfig;
using tse::ml_client::MlScoringWorker;
using tse::ml_client::MlScoringWorkerConfig;
using tse::ml_client::testutil::TestHttpServer;
using tse::orderbook::OrderBook;
using tse::pipeline::CollectingAlertSink;

namespace {

Order make_new(const std::string& id, const std::string& account, const std::string& instrument, int64_t ts,
               int64_t qty = 100) {
    Order order;
    order.order_id = id;
    order.orig_order_id = id;
    order.account_id = account;
    order.instrument_id = instrument;
    order.side = Side::kBuy;
    order.price = 100.0;
    order.qty = qty;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = ts;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    return order;
}

}  // namespace

TEST(MlAnomalyDetector, NameIsMlAnomalyDetector) {
    MlScoreClientConfig client_config;
    CollectingAlertSink alert_sink;
    MlScoringWorker worker(MlScoreClient(client_config), &alert_sink);
    MlAnomalyDetector detector(&worker);
    EXPECT_EQ(detector.name(), "MlAnomalyDetector");
}

// The structural proof this class's whole design rests on: evaluate()
// never returns anything but an empty vector, even while it's actively
// submitting work to the (here, unreachable) worker -- nothing about
// calling it can make the hot path wait on the network.
TEST(MlAnomalyDetector, EvaluateAlwaysReturnsEmptyVector) {
    MlScoreClientConfig client_config;
    client_config.base_url = "http://127.0.0.1:18100";  // nothing listening -- must not matter to evaluate()
    CollectingAlertSink alert_sink;
    MlScoringWorker worker(MlScoreClient(client_config), &alert_sink);
    MlAnomalyDetector detector(&worker);

    OrderBook book("ACME");
    AccountRegistry accounts;
    for (int i = 0; i < 20; ++i) {
        Order order = make_new("O" + std::to_string(i), "ACC-1", "ACME", 1000 + i);
        book.apply(order);
        auto alerts = detector.evaluate(book, DetectorEvent{order}, accounts);
        EXPECT_TRUE(alerts.empty());
    }
}

TEST(MlAnomalyDetector, IgnoresExecutionEvents) {
    MlScoreClientConfig client_config;
    CollectingAlertSink alert_sink;
    MlScoringWorker worker(MlScoreClient(client_config), &alert_sink);
    MlAnomalyDetector detector(&worker);

    OrderBook book("ACME");
    AccountRegistry accounts;
    Execution execution;
    execution.trade_id = "E1";
    execution.order_id = "O1";
    execution.account_id = "ACC-1";
    execution.instrument_id = "ACME";
    execution.side = Side::kBuy;
    execution.price = 100.0;
    execution.qty = 100;
    execution.timestamp_ns = 1000;
    execution.venue = "SIM";

    auto alerts = detector.evaluate(book, DetectorEvent{execution}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(MlAnomalyDetector, SubmitsExpectedFeaturesOnceMinimumOrdersReached) {
    std::mutex mutex;
    std::vector<std::string> captured_bodies;
    TestHttpServer server(18101);
    server.raw().Post("/score", [&](const httplib::Request& req, httplib::Response& res) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            captured_bodies.push_back(req.body);
        }
        res.set_content(R"({"anomaly_score":0.0,"model_version":"v1"})", "application/json");
    });

    MlScoreClientConfig client_config;
    client_config.base_url = "http://127.0.0.1:18101";
    CollectingAlertSink alert_sink;
    MlScoringWorkerConfig worker_config;
    worker_config.queue_capacity = 64;
    MlScoringWorker worker(MlScoreClient(client_config), &alert_sink, worker_config);

    MlAnomalyDetectorConfig detector_config;
    detector_config.min_orders_before_submit = 5;
    detector_config.submit_every_n_orders = 5;
    MlAnomalyDetector detector(&worker, detector_config);

    OrderBook book("ACME");
    AccountRegistry accounts;
    for (int i = 0; i < 5; ++i) {
        Order order = make_new("O" + std::to_string(i), "ACC-1", "ACME", 1000 + i, 100);
        book.apply(order);
        detector.evaluate(book, DetectorEvent{order}, accounts);
    }

    std::atomic<bool> stop{false};
    std::thread worker_thread([&] { worker.run(stop); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true, std::memory_order_release);
    worker_thread.join();

    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(captured_bodies.size(), 1u)
        << "expected exactly one submission at the 5th order (both the minimum and the periodic boundary)";
    EXPECT_NE(captured_bodies[0].find(R"("order_count":5)"), std::string::npos);
    EXPECT_NE(captured_bodies[0].find(R"("total_qty":500)"), std::string::npos);  // 5 orders * qty 100
    EXPECT_NE(captured_bodies[0].find(R"("avg_qty":100)"), std::string::npos);
    EXPECT_NE(captured_bodies[0].find(R"("account_id":"ACC-1")"), std::string::npos);
}

// A fired alert must carry model_version as its own structured field, not
// just folded into the free-text evidence string -- this is what lets
// Phase 8's persistence layer store and query it directly instead of
// parsing it back out of evidence.
TEST(MlAnomalyDetector, FiredAlertCarriesStructuredModelVersion) {
    TestHttpServer server(18104);
    server.raw().Post("/score", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"anomaly_score":0.95,"model_version":"isoforest-abc123"})", "application/json");
    });

    MlScoreClientConfig client_config;
    client_config.base_url = "http://127.0.0.1:18104";
    CollectingAlertSink alert_sink;
    MlScoringWorkerConfig worker_config;
    worker_config.alert_threshold = 0.7;
    MlScoringWorker worker(MlScoreClient(client_config), &alert_sink, worker_config);
    MlAnomalyDetectorConfig detector_config;
    detector_config.min_orders_before_submit = 5;
    detector_config.submit_every_n_orders = 5;
    MlAnomalyDetector detector(&worker, detector_config);

    OrderBook book("ACME");
    AccountRegistry accounts;
    for (int i = 0; i < 5; ++i) {
        Order order = make_new("O" + std::to_string(i), "ACC-1", "ACME", 1000 + i);
        book.apply(order);
        detector.evaluate(book, DetectorEvent{order}, accounts);
    }

    std::atomic<bool> stop{false};
    std::thread worker_thread([&] { worker.run(stop); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true, std::memory_order_release);
    worker_thread.join();

    auto alerts = alert_sink.alerts();
    ASSERT_EQ(alerts.size(), 1u);
    ASSERT_TRUE(alerts[0].model_version.has_value())
        << "MlAnomalyDetector-sourced alerts must populate the structured model_version field";
    EXPECT_EQ(*alerts[0].model_version, "isoforest-abc123");
    // Free-text evidence still carries it too, for readability -- the
    // structured field is additive, not a replacement.
    EXPECT_NE(alerts[0].evidence.find("model_version=isoforest-abc123"), std::string::npos);
}

TEST(MlAnomalyDetector, DoesNotSubmitBeforeMinimumOrdersReached) {
    std::atomic<int> request_count{0};
    TestHttpServer server(18102);
    server.raw().Post("/score", [&](const httplib::Request&, httplib::Response& res) {
        request_count.fetch_add(1, std::memory_order_relaxed);
        res.set_content(R"({"anomaly_score":0.0,"model_version":"v1"})", "application/json");
    });

    MlScoreClientConfig client_config;
    client_config.base_url = "http://127.0.0.1:18102";
    CollectingAlertSink alert_sink;
    MlScoringWorker worker(MlScoreClient(client_config), &alert_sink);
    MlAnomalyDetectorConfig detector_config;
    detector_config.min_orders_before_submit = 10;
    MlAnomalyDetector detector(&worker, detector_config);

    OrderBook book("ACME");
    AccountRegistry accounts;
    for (int i = 0; i < 4; ++i) {
        Order order = make_new("O" + std::to_string(i), "ACC-1", "ACME", 1000 + i);
        book.apply(order);
        detector.evaluate(book, DetectorEvent{order}, accounts);
    }

    std::atomic<bool> stop{false};
    std::thread worker_thread([&] { worker.run(stop); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop.store(true, std::memory_order_release);
    worker_thread.join();

    EXPECT_EQ(request_count.load(), 0);
}

TEST(MlAnomalyDetector, TracksSeparateWindowsPerAccountInstrumentPair) {
    std::mutex mutex;
    std::vector<std::string> captured_bodies;
    TestHttpServer server(18103);
    server.raw().Post("/score", [&](const httplib::Request& req, httplib::Response& res) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            captured_bodies.push_back(req.body);
        }
        res.set_content(R"({"anomaly_score":0.0,"model_version":"v1"})", "application/json");
    });

    MlScoreClientConfig client_config;
    client_config.base_url = "http://127.0.0.1:18103";
    CollectingAlertSink alert_sink;
    MlScoringWorkerConfig worker_config;
    worker_config.queue_capacity = 64;
    MlScoringWorker worker(MlScoreClient(client_config), &alert_sink, worker_config);
    MlAnomalyDetectorConfig detector_config;
    detector_config.min_orders_before_submit = 3;
    detector_config.submit_every_n_orders = 3;
    MlAnomalyDetector detector(&worker, detector_config);

    OrderBook book("ACME");
    AccountRegistry accounts;
    // ACC-1 gets 3 orders (reaches its own threshold); ACC-2 gets only 1
    // (must not accidentally share ACC-1's count).
    for (int i = 0; i < 3; ++i) {
        Order order = make_new("A" + std::to_string(i), "ACC-1", "ACME", 1000 + i);
        book.apply(order);
        detector.evaluate(book, DetectorEvent{order}, accounts);
    }
    Order other = make_new("B0", "ACC-2", "ACME", 2000);
    book.apply(other);
    detector.evaluate(book, DetectorEvent{other}, accounts);

    std::atomic<bool> stop{false};
    std::thread worker_thread([&] { worker.run(stop); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true, std::memory_order_release);
    worker_thread.join();

    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(captured_bodies.size(), 1u);  // only ACC-1 reached its threshold
    EXPECT_NE(captured_bodies[0].find(R"("account_id":"ACC-1")"), std::string::npos);
    EXPECT_NE(captured_bodies[0].find(R"("order_count":3)"), std::string::npos);
}
