#pragma once

#include <httplib.h>

#include <chrono>
#include <thread>

namespace tse::ml_client::testutil {

// Minimal in-process HTTP test server: starts listening on construction,
// stops and joins on destruction. Test-only, mirrors the spirit of
// cpp/tests/fix/fix_session_test_fixture.hpp — a genuine socket and a
// genuine cpp-httplib server, not a mock object, so what MlScoreClient
// exercises against it is the real HTTP path.
class TestHttpServer {
public:
    explicit TestHttpServer(int port) : port_(port) {
        thread_ = std::thread([this] { server_.listen("127.0.0.1", port_); });
        for (int i = 0; i < 200 && !server_.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~TestHttpServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    TestHttpServer(const TestHttpServer&) = delete;
    TestHttpServer& operator=(const TestHttpServer&) = delete;

    httplib::Server& raw() { return server_; }

private:
    int port_;
    httplib::Server server_;
    std::thread thread_;
};

}  // namespace tse::ml_client::testutil
