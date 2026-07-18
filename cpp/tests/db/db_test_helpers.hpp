#pragma once

#include <memory>

#include "alert_store.hpp"

namespace tse::db::testutil {

// Returns a ready-to-use AlertStore (schema applied, tables truncated) or
// nullptr if TimescaleDB isn't reachable. Callers GTEST_SKIP() in that case
// -- matches this project's Kafka-broker-skip precedent
// (cpp/tests/ingestion/kafka_replay_test.cpp): these are integration tests
// against a real external service, not pure unit tests, and "the service
// isn't running in this environment" isn't a test failure.
inline std::unique_ptr<AlertStore> connect_or_skip() {
    try {
        auto store = std::make_unique<AlertStore>();
        store->apply_schema();
        store->truncate_all();
        return store;
    } catch (const std::exception&) {
        return nullptr;
    }
}

}  // namespace tse::db::testutil
