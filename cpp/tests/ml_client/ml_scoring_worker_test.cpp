#include "ml_scoring_worker.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "alert_sink.hpp"
#include "test_http_server.hpp"

using tse::ml_client::MlScoreClient;
using tse::ml_client::MlScoreClientConfig;
using tse::ml_client::MlScoringWorker;
using tse::ml_client::MlScoringWorkerConfig;
using tse::ml_client::ScoringRequest;
using tse::ml_client::testutil::TestHttpServer;
using tse::pipeline::CollectingAlertSink;

namespace {

ScoringRequest make_request(const std::string& account_id) {
    return ScoringRequest{account_id, "ACME", {{"order_count", 10.0}}, 1000};
}

// Runs the worker on its own thread for a bounded window, then stops it --
// long enough for the small number of requests these tests submit to
// drain even under sanitizer instrumentation.
void run_worker_briefly(MlScoringWorker& worker, std::chrono::milliseconds duration = std::chrono::milliseconds(300)) {
    std::atomic<bool> stop{false};
    std::thread worker_thread([&] { worker.run(stop); });
    std::this_thread::sleep_for(duration);
    stop.store(true, std::memory_order_release);
    worker_thread.join();
}

}  // namespace

TEST(MlScoringWorker, SuccessfulHighScoreProducesAlert) {
    TestHttpServer server(18090);
    server.raw().Post("/score", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"anomaly_score":0.9,"model_version":"v1"})", "application/json");
    });

    MlScoreClientConfig client_config;
    client_config.base_url = "http://127.0.0.1:18090";
    CollectingAlertSink alert_sink;
    MlScoringWorkerConfig worker_config;
    worker_config.alert_threshold = 0.7;
    MlScoringWorker worker(MlScoreClient(client_config), &alert_sink, worker_config);

    worker.submit(make_request("ACC-1"));
    run_worker_briefly(worker);

    EXPECT_EQ(worker.requests_scored(), 1u);
    EXPECT_EQ(worker.requests_alerted(), 1u);
    EXPECT_EQ(worker.requests_failed(), 0u);
    auto alerts = alert_sink.alerts();
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_EQ(alerts[0].detector_name, "MlAnomalyDetector");
    EXPECT_DOUBLE_EQ(alerts[0].score, 0.9);
    ASSERT_EQ(alerts[0].account_ids.size(), 1u);
    EXPECT_EQ(alerts[0].account_ids[0], "ACC-1");
}

TEST(MlScoringWorker, LowScoreDoesNotProduceAlert) {
    TestHttpServer server(18091);
    server.raw().Post("/score", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"anomaly_score":0.1,"model_version":"v1"})", "application/json");
    });

    MlScoreClientConfig client_config;
    client_config.base_url = "http://127.0.0.1:18091";
    CollectingAlertSink alert_sink;
    MlScoringWorkerConfig worker_config;
    worker_config.alert_threshold = 0.7;
    MlScoringWorker worker(MlScoreClient(client_config), &alert_sink, worker_config);

    worker.submit(make_request("ACC-1"));
    run_worker_briefly(worker);

    EXPECT_EQ(worker.requests_scored(), 1u);
    EXPECT_EQ(worker.requests_alerted(), 0u);
    EXPECT_TRUE(alert_sink.alerts().empty());
}

// The central graceful-degradation proof at the worker level: a request
// that can never connect is counted as failed, never crashes the worker
// thread, and never produces an alert.
TEST(MlScoringWorker, FailedRequestIsCountedNotAlertedNotCrashed) {
    MlScoreClientConfig client_config;
    client_config.base_url = "http://127.0.0.1:18092";  // nothing listening
    client_config.connect_timeout_ms = 100;
    CollectingAlertSink alert_sink;
    MlScoringWorker worker(MlScoreClient(client_config), &alert_sink);

    worker.submit(make_request("ACC-1"));
    run_worker_briefly(worker);

    EXPECT_EQ(worker.requests_scored(), 0u);
    EXPECT_EQ(worker.requests_failed(), 1u);
    EXPECT_TRUE(alert_sink.alerts().empty());
}

// submit() must never block, even when the queue is saturated and nothing
// is draining it -- the exact property the hot path depends on.
TEST(MlScoringWorker, SubmitNeverBlocksWhenQueueIsFullAndNothingIsDraining) {
    MlScoreClientConfig client_config;
    client_config.base_url = "http://127.0.0.1:18093";  // never used -- worker never runs in this test
    MlScoringWorkerConfig worker_config;
    worker_config.queue_capacity = 2;  // smallest legal capacity (see cpp/ingestion/spsc_ring_buffer.hpp)
    CollectingAlertSink alert_sink;
    MlScoringWorker worker(MlScoreClient(client_config), &alert_sink, worker_config);

    for (int i = 0; i < 1000; ++i) {
        worker.submit(make_request("ACC-1"));
    }
    EXPECT_GT(worker.requests_dropped(), 0u);
}

TEST(MlScoringWorker, MultipleRequestsAreAllProcessed) {
    std::atomic<int> request_count{0};
    TestHttpServer server(18094);
    server.raw().Post("/score", [&](const httplib::Request&, httplib::Response& res) {
        request_count.fetch_add(1, std::memory_order_relaxed);
        res.set_content(R"({"anomaly_score":0.2,"model_version":"v1"})", "application/json");
    });

    MlScoreClientConfig client_config;
    client_config.base_url = "http://127.0.0.1:18094";
    CollectingAlertSink alert_sink;
    MlScoringWorkerConfig worker_config;
    worker_config.queue_capacity = 64;
    MlScoringWorker worker(MlScoreClient(client_config), &alert_sink, worker_config);

    for (int i = 0; i < 10; ++i) {
        worker.submit(make_request("ACC-" + std::to_string(i)));
    }
    run_worker_briefly(worker);

    EXPECT_EQ(worker.requests_scored(), 10u);
    EXPECT_EQ(request_count.load(), 10);
}
