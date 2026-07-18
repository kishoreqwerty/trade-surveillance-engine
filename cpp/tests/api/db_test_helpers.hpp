#pragma once

#include <memory>

#include "alert_store.hpp"

namespace tse::db::testutil {

// Small, deliberate duplicate of cpp/tests/db/db_test_helpers.hpp -- a
// handful of lines, not worth sharing across test directories for. See
// that file for the full rationale: returns a ready-to-use AlertStore
// (schema applied, tables truncated) or nullptr if TimescaleDB isn't
// reachable, matching this project's Kafka-broker-skip precedent
// (cpp/tests/ingestion/kafka_replay_test.cpp).
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
