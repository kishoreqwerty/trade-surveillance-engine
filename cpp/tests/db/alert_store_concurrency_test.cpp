#include "alert_store.hpp"

#include <atomic>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "db_test_helpers.hpp"

using tse::db::AlertStore;
using tse::db::testutil::connect_or_skip;
using tse::detectors::Alert;

namespace {

Alert make_alert(int thread_index, int i) {
    Alert alert;
    alert.detector_name = "WashTradeDetector";
    alert.score = 1.0;
    alert.instrument_id = "ACME";
    alert.account_ids = {"ACC-T" + std::to_string(thread_index) + "-" + std::to_string(i)};
    alert.order_ids = {"O-T" + std::to_string(thread_index) + "-" + std::to_string(i)};
    alert.window_start_ns = 1'700'000'000'000'000'000LL;
    alert.window_end_ns = 1'700'000'000'100'000'000LL;
    alert.evidence = "concurrency test";
    return alert;
}

}  // namespace

// Regression for a real crash found running cpp/api/main.cpp's live demo
// server under ordinary concurrent dashboard polling:
// `pqxx::usage_error: Started new transaction while transaction was still
// active`. Root cause: AlertStore wrapped exactly one pqxx::connection
// (not thread-safe for concurrent use -- libpqxx's own documented
// constraint) with no internal synchronization, while api/'s Crow server
// runs multithreaded() and pipeline/'s consumer thread writes through
// DbAlertSink into the very same AlertStore instance concurrently. This
// test reproduces that shape directly: several threads simultaneously
// inserting and querying the same AlertStore, mixing reads and writes
// exactly like the live demo server's multithreaded HTTP handlers +
// pipeline consumer thread do. Must be run under TSan specifically per
// CLAUDE.md's rule for concurrency code -- see README.md.
TEST(ConcurrentAlertStoreTest, ConcurrentInsertsAndQueriesFromMultipleThreadsDoNotCrashAndAllRowsLand) {
    auto store = connect_or_skip();
    if (!store) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

    constexpr int kThreads = 8;
    constexpr int kInsertsPerThread = 15;

    std::vector<std::thread> threads;
    std::mutex exceptions_mutex;
    std::vector<std::string> exceptions;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            try {
                for (int i = 0; i < kInsertsPerThread; ++i) {
                    store->insert_alert(make_alert(t, i));
                    // Interleave reads with writes -- exactly the
                    // concurrent read+write mix that crashed the live
                    // demo server (dashboard polling /api/alerts while
                    // the pipeline consumer thread writes new alerts).
                    (void)store->list_recent_alerts(5);
                    (void)store->query_alerts_by_detector("WashTradeDetector");
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(exceptions_mutex);
                exceptions.push_back(e.what());
            }
        });
    }
    for (auto& thread : threads) thread.join();

    EXPECT_TRUE(exceptions.empty()) << "at least one thread saw an exception -- first: "
                                     << (exceptions.empty() ? "" : exceptions.front());

    std::vector<tse::db::StoredAlert> all = store->query_alerts_by_detector("WashTradeDetector");
    EXPECT_EQ(all.size(), static_cast<size_t>(kThreads * kInsertsPerThread))
        << "some inserts were lost or duplicated under concurrent access";
}
