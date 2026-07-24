#pragma once

#include <cstdint>
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

// One page of list_alerts_paginated()'s result, plus the real (COUNT(*),
// not estimated) total matching the same filter -- what lets a caller
// render "page X of Y" and a real total-alert-count honestly, not from a
// guess.
struct PaginatedAlerts {
    std::vector<StoredAlert> alerts;
    int64_t total_count{0};
};

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
    //
    // query_alerts_by_account()/query_alerts_by_detector() order by
    // alert_id (real insertion order), not window_start_ns (the synthetic
    // event clock, which cpp/api/main.cpp's demo feed loop can make
    // regress backward at session boundaries -- see list_recent_alerts()
    // below and cpp/pipeline/README.md). query_alerts_by_time_range()
    // still orders by window_start_ns deliberately: its start/end
    // parameters are themselves synthetic-time bounds the caller chose, so
    // ordering the filtered result by that same field is the caller's own
    // request, not an implicit recency proxy.
    std::vector<StoredAlert> query_alerts_by_time_range(int64_t window_start_ns, int64_t window_end_ns) const;
    std::vector<StoredAlert> query_alerts_by_account(const std::string& account_id) const;
    std::vector<StoredAlert> query_alerts_by_detector(const std::string& detector_name) const;

    // Phase 9 additions -- api/'s default "list alerts" (no filter given)
    // and single-alert-lookup endpoints, plus the write side of case
    // management (api/'s compliance action endpoints).
    //
    // Orders by alert_id DESC (real insertion order), not window_start_ns
    // DESC -- see .cpp for the full reasoning. "Most recent 250" here means
    // "most recently inserted," which is what a compliance analyst
    // actually wants from a live feed and what api/'s dashboard callers
    // assume; it is not guaranteed to be "highest window_start_ns."
    std::vector<StoredAlert> list_recent_alerts(int limit) const;

    // ALERTS tab's paginated list query, added for real offset-based
    // pagination (a page footer with real prev/next and an honest total
    // count, not client-side slicing of one big fetched window).
    // detector_name/status: nullopt means "no filter on that field" --
    // both real WHERE-clause filters, not client-side. Free-text search
    // (instrument/account substring) deliberately stays client-side, out
    // of this method's scope -- see dashboard/src/tabs/AlertsTab.tsx's own
    // comment for why. Orders by alert_id DESC, same real-insertion-order
    // reasoning as list_recent_alerts() above.
    PaginatedAlerts list_alerts_paginated(std::optional<std::string> detector_name, std::optional<std::string> status,
                                           int limit, int offset) const;
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

    // A genuine live-liveness check for api/'s status tile -- deliberately
    // NOT conn_->is_open(), which only reflects libpqxx's cached status
    // from the last I/O attempt (it wraps libpq's PQstatus(), which is
    // updated lazily on actual traffic, not polled). If TimescaleDB drops
    // mid-session and nothing has queried since, is_open() keeps reporting
    // true. This runs a trivial real round trip instead, so a genuine
    // outage is caught on the next call rather than reported stale.
    bool is_connected() const;

private:
    std::unique_ptr<pqxx::connection> conn_;
    // mutable: several callers below (query_*, get_alert, list_recent_alerts)
    // are logically read-only and declared const, but still need to guard
    // the one shared connection like every other method.
    mutable std::mutex mutex_;
};

}  // namespace tse::db
