#pragma once

#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include <crow.h>

#include "alert_store.hpp"
#include "api_server.hpp"
#include "live_book_registry.hpp"

namespace tse::api::testutil {

// Runs a real Crow app on a background thread -- a genuine HTTP server and
// socket, not a mocked router -- so tests exercise the real
// request-parsing/routing/response-serialization path, mirroring
// cpp/tests/ml_client/test_http_server.hpp's TestHttpServer for the
// server-under-test side of this project's other real-socket integration
// tests.
class TestApiServer {
public:
    TestApiServer(int port, tse::db::AlertStore* store, tse::api::LiveBookRegistry* book_registry,
                  std::string evaluation_results_dir = "cpp/harness/results")
        : port_(port) {
        tse::api::register_routes(app_, store, book_registry, {}, std::move(evaluation_results_dir));
        thread_ = std::thread([this] { app_.port(port_).multithreaded().run(); });
        app_.wait_for_server_start();
    }

    ~TestApiServer() {
        app_.stop();
        if (thread_.joinable()) thread_.join();
    }

    TestApiServer(const TestApiServer&) = delete;
    TestApiServer& operator=(const TestApiServer&) = delete;

    int port() const { return port_; }

private:
    int port_;
    tse::api::App app_;
    std::thread thread_;
};

}  // namespace tse::api::testutil
