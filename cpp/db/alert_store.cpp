#include "alert_store.hpp"

#include <fstream>
#include <sstream>

#include <pqxx/pqxx>

namespace tse::db {

namespace {

// Postgres text-array literal encode/decode -- libpqxx has no built-in
// std::vector<std::string> <-> TEXT[] binding, and this project doesn't
// pull in a general-purpose array-handling layer for exactly two columns
// (account_ids, order_ids), matching the same "hand-roll exactly the
// encoding a fixed, known shape needs" precedent as
// cpp/ml_client/ml_json.cpp and cpp/simulator/serialization/json_writer.cpp.
//
// Elements are always double-quoted on encode (simplest correct choice --
// Postgres accepts quoted elements unconditionally, so there's no need to
// duplicate its own "quote only if necessary" logic). Decode handles both
// quoted and unquoted elements, since Postgres's own array_out doesn't
// quote elements that don't need it (e.g. plain alphanumeric order IDs),
// and a round-trip through a real database must handle what it actually
// sends back, not just what this code happens to write.
std::string to_pg_text_array(const std::vector<std::string>& items) {
    std::string result = "{";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) result += ',';
        result += '"';
        for (char c : items[i]) {
            if (c == '"' || c == '\\') result += '\\';
            result += c;
        }
        result += '"';
    }
    result += '}';
    return result;
}

std::vector<std::string> from_pg_text_array(const std::string& text) {
    std::vector<std::string> result;
    if (text.size() < 2 || text.front() != '{' || text.back() != '}') return result;
    std::size_t i = 1;
    const std::size_t end = text.size() - 1;
    while (i < end) {
        if (text[i] == ',') {
            ++i;
            continue;
        }
        std::string element;
        if (text[i] == '"') {
            ++i;
            while (i < end && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < end) {
                    element += text[i + 1];
                    i += 2;
                } else {
                    element += text[i];
                    ++i;
                }
            }
            ++i;  // closing quote
        } else {
            while (i < end && text[i] != ',') {
                element += text[i];
                ++i;
            }
        }
        result.push_back(std::move(element));
    }
    return result;
}

std::vector<StoredAlert> rows_to_stored_alerts(const pqxx::result& rows) {
    std::vector<StoredAlert> result;
    result.reserve(rows.size());
    for (const auto& row : rows) {
        StoredAlert stored;
        stored.alert_id = row["alert_id"].as<int64_t>();
        stored.alert.detector_name = row["detector_name"].as<std::string>();
        stored.alert.score = row["score"].as<double>();
        stored.alert.instrument_id = row["instrument_id"].as<std::string>();
        stored.alert.account_ids = from_pg_text_array(row["account_ids"].as<std::string>());
        stored.alert.order_ids = from_pg_text_array(row["order_ids"].as<std::string>());
        stored.alert.window_start_ns = row["window_start_ns"].as<int64_t>();
        stored.alert.window_end_ns = row["window_end_ns"].as<int64_t>();
        stored.alert.evidence = row["evidence"].as<std::string>();
        if (!row["model_version"].is_null()) {
            stored.alert.model_version = row["model_version"].as<std::string>();
        }
        if (!row["book_snapshot_sequence"].is_null()) {
            stored.alert.book_snapshot_sequence = row["book_snapshot_sequence"].as<int64_t>();
        }
        result.push_back(std::move(stored));
    }
    return result;
}

constexpr const char* kAlertColumns =
    "alert_id, detector_name, score, instrument_id, account_ids, order_ids, "
    "window_start_ns, window_end_ns, evidence, model_version, book_snapshot_sequence";

}  // namespace

AlertStore::AlertStore(const DbConfig& config) : conn_(std::make_unique<pqxx::connection>(config.connection_string())) {}

AlertStore::~AlertStore() = default;

void AlertStore::apply_schema() {
#ifndef TSE_DB_SCHEMA_PATH
#error "TSE_DB_SCHEMA_PATH must be defined by CMake -- see cpp/db/CMakeLists.txt"
#endif
    std::ifstream file(TSE_DB_SCHEMA_PATH);
    std::stringstream buffer;
    buffer << file.rdbuf();

    pqxx::work txn(*conn_);
    txn.exec(buffer.str());
    txn.commit();
}

// Every INSERT below reuses one bind parameter twice: once bound straight
// to a BIGINT column, once inside to_timestamp($n::double precision / ...)
// to derive that row's event_time. Both occurrences carry an explicit
// ::bigint / ::double precision cast -- found necessary against the real
// database (not something the unit-level tests without a live Postgres
// caught): leaving either occurrence uncast makes Postgres's parameter
// type inference try to unify the parameter to a single type across the
// whole statement and fail with "inconsistent types deduced for parameter
// $n" the moment the two inferred types disagree.
void AlertStore::insert_order(const tse::fix::Order& order) {
    pqxx::work txn(*conn_);
    txn.exec_params(
        "INSERT INTO orders (order_id, orig_order_id, account_id, instrument_id, side, price, qty, "
        "order_type, status, venue, timestamp_ns, event_time) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11::bigint, to_timestamp($11::double precision / 1000000000.0))",
        order.order_id, order.orig_order_id, order.account_id, order.instrument_id,
        tse::fix::to_string(order.side), order.price, order.qty, tse::fix::to_string(order.order_type),
        tse::fix::to_string(order.status), order.venue, order.timestamp_ns);
    txn.commit();
}

void AlertStore::insert_execution(const tse::fix::Execution& execution) {
    pqxx::work txn(*conn_);
    txn.exec_params(
        "INSERT INTO trades (trade_id, order_id, account_id, counterparty_account_id, instrument_id, side, "
        "price, qty, venue, timestamp_ns, event_time) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10::bigint, to_timestamp($10::double precision / 1000000000.0))",
        execution.trade_id, execution.order_id, execution.account_id, execution.counterparty_account_id,
        execution.instrument_id, tse::fix::to_string(execution.side), execution.price, execution.qty,
        execution.venue, execution.timestamp_ns);
    txn.commit();
}

int64_t AlertStore::insert_alert(const tse::detectors::Alert& alert) {
    pqxx::work txn(*conn_);
    const std::string account_ids_literal = to_pg_text_array(alert.account_ids);
    const std::string order_ids_literal = to_pg_text_array(alert.order_ids);
    pqxx::row r = txn.exec_params1(
        "INSERT INTO alerts (detector_name, score, instrument_id, account_ids, order_ids, "
        "window_start_ns, window_end_ns, evidence, model_version, book_snapshot_sequence, event_time) "
        "VALUES ($1,$2,$3,$4,$5,$6,$7::bigint,$8,$9,$10, to_timestamp($7::double precision / 1000000000.0)) "
        "RETURNING alert_id",
        alert.detector_name, alert.score, alert.instrument_id, account_ids_literal, order_ids_literal,
        alert.window_start_ns, alert.window_end_ns, alert.evidence, alert.model_version,
        alert.book_snapshot_sequence);
    txn.commit();
    return r["alert_id"].as<int64_t>();
}

std::vector<StoredAlert> AlertStore::query_alerts_by_time_range(int64_t window_start_ns, int64_t window_end_ns) const {
    pqxx::work txn(*conn_);
    pqxx::result rows = txn.exec_params(std::string("SELECT ") + kAlertColumns +
                                             " FROM alerts WHERE window_start_ns >= $1 AND window_start_ns <= $2 "
                                             "ORDER BY window_start_ns",
                                         window_start_ns, window_end_ns);
    txn.commit();
    return rows_to_stored_alerts(rows);
}

std::vector<StoredAlert> AlertStore::query_alerts_by_account(const std::string& account_id) const {
    pqxx::work txn(*conn_);
    pqxx::result rows = txn.exec_params(std::string("SELECT ") + kAlertColumns +
                                             " FROM alerts WHERE account_ids @> ARRAY[$1]::text[] "
                                             "ORDER BY window_start_ns",
                                         account_id);
    txn.commit();
    return rows_to_stored_alerts(rows);
}

std::vector<StoredAlert> AlertStore::query_alerts_by_detector(const std::string& detector_name) const {
    pqxx::work txn(*conn_);
    pqxx::result rows = txn.exec_params(
        std::string("SELECT ") + kAlertColumns + " FROM alerts WHERE detector_name = $1 ORDER BY window_start_ns",
        detector_name);
    txn.commit();
    return rows_to_stored_alerts(rows);
}

void AlertStore::truncate_all() {
    pqxx::work txn(*conn_);
    txn.exec("TRUNCATE orders, trades, alerts");
    txn.commit();
}

}  // namespace tse::db
