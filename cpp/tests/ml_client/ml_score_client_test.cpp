#include "ml_score_client.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "test_http_server.hpp"

using tse::ml_client::MlScoreClient;
using tse::ml_client::MlScoreClientConfig;
using tse::ml_client::ScoringRequest;
using tse::ml_client::ScoringResult;
using tse::ml_client::testutil::TestHttpServer;

TEST(MlScoreClient, SuccessfulScoreReturnsResult) {
    TestHttpServer server(18081);
    server.raw().Post("/score", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"anomaly_score":0.42,"model_version":"test-v1"})", "application/json");
    });

    MlScoreClientConfig config;
    config.base_url = "http://127.0.0.1:18081";
    MlScoreClient client(config);

    std::optional<ScoringResult> result = client.score(ScoringRequest{"ACC-1", "ACME", {}, 1000});
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->anomaly_score, 0.42);
    EXPECT_EQ(result->model_version, "test-v1");
}

// The core "must degrade gracefully" proof at the client level: a server
// that's slower than the configured timeout never makes score() throw,
// hang past the timeout, or return anything other than nullopt.
TEST(MlScoreClient, TimeoutReturnsNulloptNotThrow) {
    TestHttpServer server(18082);
    server.raw().Post("/score", [](const httplib::Request&, httplib::Response& res) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));  // well past the client's 100ms below
        res.set_content(R"({"anomaly_score":0.1,"model_version":"v1"})", "application/json");
    });

    MlScoreClientConfig config;
    config.base_url = "http://127.0.0.1:18082";
    config.read_timeout_ms = 100;
    MlScoreClient client(config);

    std::optional<ScoringResult> result;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(result = client.score(ScoringRequest{"ACC-1", "ACME", {}, 1000}));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(result.has_value());
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500)
        << "score() should have returned once the read timeout fired, not waited for the slow server";
}

TEST(MlScoreClient, ConnectionRefusedReturnsNulloptNotThrow) {
    // Nothing listens on this port.
    MlScoreClientConfig config;
    config.base_url = "http://127.0.0.1:18083";
    config.connect_timeout_ms = 200;
    MlScoreClient client(config);

    std::optional<ScoringResult> result;
    EXPECT_NO_THROW(result = client.score(ScoringRequest{"ACC-1", "ACME", {}, 1000}));
    EXPECT_FALSE(result.has_value());
}

TEST(MlScoreClient, NonOkStatusReturnsNullopt) {
    TestHttpServer server(18084);
    server.raw().Post("/score", [](const httplib::Request&, httplib::Response& res) {
        res.status = 500;
        res.set_content("internal error", "text/plain");
    });

    MlScoreClientConfig config;
    config.base_url = "http://127.0.0.1:18084";
    MlScoreClient client(config);

    EXPECT_FALSE(client.score(ScoringRequest{"ACC-1", "ACME", {}, 1000}).has_value());
}

TEST(MlScoreClient, MalformedResponseBodyReturnsNullopt) {
    TestHttpServer server(18085);
    server.raw().Post("/score", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("not valid json", "application/json");
    });

    MlScoreClientConfig config;
    config.base_url = "http://127.0.0.1:18085";
    MlScoreClient client(config);

    EXPECT_FALSE(client.score(ScoringRequest{"ACC-1", "ACME", {}, 1000}).has_value());
}

// Proves MlScoreClient carries no persistent broken state across a
// failure: the SAME client instance that just got connection-refused
// against a dead endpoint succeeds again once something starts listening
// on that port -- no circuit-breaker flag, no cached bad connection, no
// explicit reset call needed. This is the client-level half of the
// recovery proof; graceful_degradation_test.cpp's "recovered" scenario is
// the full end-to-end version (real killed subprocess, real restart,
// through MlScoringWorker, ml_scored actually incrementing).
TEST(MlScoreClient, RecoversAfterServerBecomesAvailableOnSameClientInstance) {
    constexpr int kPort = 18087;

    MlScoreClientConfig config;
    config.base_url = "http://127.0.0.1:" + std::to_string(kPort);
    config.connect_timeout_ms = 200;
    MlScoreClient client(config);

    // Nothing listens on kPort yet -- must fail cleanly.
    EXPECT_FALSE(client.score(ScoringRequest{"ACC-1", "ACME", {}, 1000}).has_value());

    // Now bring a real server up on the same port and reuse the same
    // client instance.
    {
        TestHttpServer server(kPort);
        server.raw().Post("/score", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"({"anomaly_score":0.55,"model_version":"recovered-v1"})", "application/json");
        });

        std::optional<ScoringResult> result = client.score(ScoringRequest{"ACC-1", "ACME", {}, 1000});
        ASSERT_TRUE(result.has_value()) << "the same client instance should succeed once the server is reachable";
        EXPECT_DOUBLE_EQ(result->anomaly_score, 0.55);
        EXPECT_EQ(result->model_version, "recovered-v1");
    }
}

TEST(MlScoreClient, RequestBodyActuallyReachesTheServer) {
    std::string captured_body;
    TestHttpServer server(18086);
    server.raw().Post("/score", [&](const httplib::Request& req, httplib::Response& res) {
        captured_body = req.body;
        res.set_content(R"({"anomaly_score":0.0,"model_version":"v1"})", "application/json");
    });

    MlScoreClientConfig config;
    config.base_url = "http://127.0.0.1:18086";
    MlScoreClient client(config);

    ScoringRequest request{"ACC-42", "BETA", {{"order_count", 7.0}}, 1000};
    client.score(request);

    EXPECT_NE(captured_body.find(R"("account_id":"ACC-42")"), std::string::npos);
    EXPECT_NE(captured_body.find(R"("instrument_id":"BETA")"), std::string::npos);
    EXPECT_NE(captured_body.find(R"("order_count":7)"), std::string::npos);
}
