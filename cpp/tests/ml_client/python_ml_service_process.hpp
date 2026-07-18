#pragma once

#include <chrono>
#include <string>

#include <sys/types.h>

namespace tse::ml_client::testutil {

// Manages a real ml_service/ uvicorn subprocess for integration testing.
// POSIX fork/exec/kill — test-only, mirrors the spirit of
// cpp/tests/fix/fix_session_test_fixture.hpp (real OS resources, not
// mocks) applied to a child process instead of a socket.
class PythonMlServiceProcess {
public:
    // port: which port uvicorn should bind on 127.0.0.1. artificial_delay_ms:
    // sets ML_SERVICE_ARTIFICIAL_DELAY_MS in the child's environment (0 = unset,
    // meaning normal/fast operation).
    explicit PythonMlServiceProcess(int port, int artificial_delay_ms = 0);
    ~PythonMlServiceProcess();

    PythonMlServiceProcess(const PythonMlServiceProcess&) = delete;
    PythonMlServiceProcess& operator=(const PythonMlServiceProcess&) = delete;

    // Polls GET /health until it responds 200 or timeout elapses. Returns
    // false on timeout — callers should GTEST_SKIP() rather than fail
    // when this happens (e.g. ml_service/.venv genuinely absent in this
    // environment), consistent with this project's Kafka-broker-skip
    // precedent (cpp/ingestion/kafka_replay_test.cpp).
    bool wait_until_ready(std::chrono::milliseconds timeout);

    // Sends SIGKILL and reaps the child immediately — simulates the
    // service crashing/being killed outright, not a graceful shutdown.
    // Idempotent: safe to call more than once, and the destructor calls
    // it too if a test doesn't.
    void kill_now();

    // True iff the process is still alive (checked via kill(pid, 0), not
    // cached state) — false after kill_now() or if the child exited on
    // its own (e.g. a startup failure).
    bool is_running() const;

private:
    pid_t pid_{-1};
    int port_;
    bool killed_{false};
};

}  // namespace tse::ml_client::testutil
