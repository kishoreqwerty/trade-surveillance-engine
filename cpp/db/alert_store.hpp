#pragma once

#include <memory>
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
// not throughput; if a future live entrypoint wires this onto the hot
// consumer thread, the async-off-the-hot-path treatment ml_client/ already
// gives ML scoring (bounded queue + background writer thread) is the
// obvious template to follow -- deliberately not built here ahead of that
// actual need. See README.md.
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

    // Test-only convenience -- never called outside cpp/tests/db/.
    void truncate_all();

private:
    std::unique_ptr<pqxx::connection> conn_;
};

}  // namespace tse::db
