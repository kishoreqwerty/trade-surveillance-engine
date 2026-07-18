#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "alert.hpp"
#include "db_config.hpp"
#include "stored_alert.hpp"
#include "types.hpp"  // tse::fix::Order, tse::fix::Execution

namespace pqxx {
class connection;
}  // namespace pqxx

namespace tse::db {

// Owns the connection to TimescaleDB and is the only class in this project
// that speaks SQL. Every write is its own transaction -- Phase 8's job is
// correctness (schema, queries, real round-trips against a real database),
// not throughput; if a future live entrypoint needs to put writes on a hot
// consumer thread, the async-off-the-hot-path treatment ml_client/ already
// gives ML scoring (bounded queue + background writer thread) is the
// obvious template to follow -- deliberately not built here ahead of that
// actual need. See README.md.
//
// Internally synchronized (mutex_ guards every method -- see .cpp): a
// single pqxx::connection is not safe for concurrent use from multiple
// threads (libpqxx's own documented constraint -- one connection carries
// one protocol/transaction state machine), and this store is shared
// between api/'s multithreaded Crow route handlers (reads) and
// pipeline/'s single consumer thread via DbAlertSink (writes). Found the
// hard way: cpp/api/main.cpp's live demo server crashed with
// `pqxx::usage_error: Started new transaction while transaction was still
// active` under ordinary concurrent dashboard polling (two panels issuing
// overlapping requests is enough) -- see README.md for the full story and
// ConcurrentAlertStoreTest for the regression coverage, run under TSan
// specifically per CLAUDE.md's rule for concurrency code. A single coarse
// mutex, not a connection pool, is the proportionate fix here: this class
// is explicitly not meant to be a high-throughput hot path (see above),
// and every method already does exactly one round trip.
class AlertStore {
public:
    explicit AlertStore(const DbConfig& config = {});
    ~AlertStore();
    AlertStore(const AlertStore&) = delete;
    AlertStore& operator=(const AlertStore&) = delete;

    // Idempotent -- every statement in schema.sql is IF NOT EXISTS. Safe to
    // call on every startup, real or test.
    void apply_schema();

    void insert_order(const tse::fix::Order& order);
    void insert_execution(const tse::fix::Execution& execution);
    // Returns the alert_id TimescaleDB assigned (schema.sql's BIGSERIAL).
    int64_t insert_alert(const tse::detectors::Alert& alert);

    // The query correctness surface the build guide's Phase 8 "Done when"
    // names explicitly: time-range, filter-by-account, filter-by-detector-
    // type. Also exactly what api/ (Phase 9) will call over REST.
    std::vector<StoredAlert> query_alerts_by_time_range(int64_t window_start_ns, int64_t window_end_ns) const;
    std::vector<StoredAlert> query_alerts_by_account(const std::string& account_id) const;
    std::vector<StoredAlert> query_alerts_by_detector(const std::string& detector_name) const;

    // Phase 9 additions -- api/'s default "list alerts" (no filter given)
    // and single-alert-lookup endpoints, plus the write side of case
    // management (api/'s compliance action endpoints).
    std::vector<StoredAlert> list_recent_alerts(int limit) const;
    std::optional<StoredAlert> get_alert(int64_t alert_id) const;
    // Throws pqxx::check_violation if new_status isn't one of schema.sql's
    // allowed values -- deliberately not re-validated against a second,
    // C++-side list of the same values, which would just be a second place
    // for that list to drift out of sync with the schema. Throws
    // std::runtime_error if alert_id matches no row (checked explicitly:
    // an UPDATE ... WHERE that matches nothing doesn't error on its own).
    void update_alert_status(int64_t alert_id, const std::string& new_status);

    // Test-only convenience -- never called outside cpp/tests/db/.
    void truncate_all();

private:
    std::unique_ptr<pqxx::connection> conn_;
    // mutable: several callers below (query_*, get_alert, list_recent_alerts)
    // are logically read-only and declared const, but still need to guard
    // the one shared connection like every other method.
    mutable std::mutex mutex_;
};

}  // namespace tse::db
