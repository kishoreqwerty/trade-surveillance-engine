#include "api_server.hpp"

#include <exception>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "json_encode.hpp"

namespace tse::api {

namespace {

crow::json::wvalue error_body(const std::string& message) {
    crow::json::wvalue out;
    out["error"] = message;
    return out;
}

std::optional<int64_t> parse_id(const std::string& text) {
    try {
        size_t consumed = 0;
        int64_t value = std::stoll(text, &consumed);
        if (consumed != text.size()) return std::nullopt;
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Reads a whole file's raw bytes, or nullopt if it doesn't exist / can't
// be opened -- used by GET /api/evaluation to load committed,
// harness-generated JSON snapshots off disk, not to build them.
std::optional<std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

}  // namespace

void register_routes(App& app, tse::db::AlertStore* store, LiveBookRegistry* book_registry,
                      tse::ml_client::MlScoreClientConfig ml_config, std::string evaluation_results_dir) {
    // Explicit headers/methods, not "*" -- the CORSHandler's own header
    // comment warns that a browser's OPTIONS preflight cache ignores a
    // wildcard Access-Control-Allow-Headers; Content-Type must be listed
    // by name for the dashboard's PATCH .../status (which sends a JSON
    // body) to actually get past preflight.
    app.get_middleware<crow::CORSHandler>()
        .global()
        .headers("Content-Type")
        .methods("GET"_method, "PATCH"_method);

    CROW_ROUTE(app, "/api/health")
    ([] {
        crow::json::wvalue out;
        out["status"] = "ok";
        return crow::response(200, out);
    });

    // GET /api/status -- backs the dashboard's KPI strip. Every field here
    // is a genuine, freshly-checked signal (see AlertStore::is_connected()
    // and MlScoreClient::health_check()'s own comments for why a cached
    // flag wasn't good enough), not a value cached at startup. detectors_*
    // is null when no live pipeline is attached to this process (mirrors
    // the order-book snapshot route's own nullable-book_registry stance).
    CROW_ROUTE(app, "/api/status")
    ([store, book_registry, ml_config] {
        crow::json::wvalue out;
        out["db_connected"] = store->is_connected();
        if (book_registry != nullptr) {
            const auto count = static_cast<int64_t>(book_registry->detector_count());
            out["detectors_active"] = count;
            out["detectors_total"] = count;
        } else {
            out["detectors_active"] = crow::json::wvalue(nullptr);
            out["detectors_total"] = crow::json::wvalue(nullptr);
        }
        tse::ml_client::MlScoreClient ml_client(ml_config);
        out["ml_service_healthy"] = ml_client.health_check();
        return crow::response(200, out);
    });

    // GET /api/alerts[?account_id=...|detector_name=...|start_ns=...&end_ns=...][&limit=N]
    // The three query params map directly onto AlertStore's three Phase 8
    // query methods -- one filter per request, not a combinator, mirroring
    // exactly the query shapes that layer exposes. No filter given at all
    // falls back to list_recent_alerts(), the "what does a compliance
    // analyst see when they first open the dashboard" case.
    //
    // GET /api/alerts?offset=N[&limit=N][&detector_name=...][&status=...]
    // ALERTS tab's paginated page-footer path, added later (Phase 12) --
    // `offset` (even "0") is the pagination-aware caller's explicit signal:
    // every existing caller above never sends it, so this branch is purely
    // additive and changes nothing about the dispatch above. Returns
    // {"alerts": [...], "total_count": N} via list_alerts_paginated(), a
    // real COUNT(*) under the same filter, not an estimate.
    CROW_ROUTE(app, "/api/alerts")
    ([store](const crow::request& req) {
        try {
            const char* account_id = req.url_params.get("account_id");
            const char* detector_name = req.url_params.get("detector_name");
            const char* start_ns_param = req.url_params.get("start_ns");
            const char* end_ns_param = req.url_params.get("end_ns");
            const char* limit_param = req.url_params.get("limit");
            const char* offset_param = req.url_params.get("offset");
            const char* status_param = req.url_params.get("status");

            if (offset_param != nullptr) {
                int limit = limit_param != nullptr ? std::stoi(limit_param) : 50;
                int offset = std::stoi(offset_param);
                std::optional<std::string> detector_opt =
                    detector_name != nullptr ? std::optional<std::string>(detector_name) : std::nullopt;
                std::optional<std::string> status_opt =
                    status_param != nullptr ? std::optional<std::string>(status_param) : std::nullopt;
                tse::db::PaginatedAlerts page = store->list_alerts_paginated(detector_opt, status_opt, limit, offset);
                return crow::response(200, encode_alerts(page.alerts, page.total_count));
            }

            std::vector<tse::db::StoredAlert> results;
            if (account_id != nullptr) {
                results = store->query_alerts_by_account(account_id);
            } else if (detector_name != nullptr) {
                results = store->query_alerts_by_detector(detector_name);
            } else if (start_ns_param != nullptr && end_ns_param != nullptr) {
                results = store->query_alerts_by_time_range(std::stoll(start_ns_param), std::stoll(end_ns_param));
            } else {
                int limit = limit_param != nullptr ? std::stoi(limit_param) : 50;
                results = store->list_recent_alerts(limit);
            }
            return crow::response(200, encode_alerts(results));
        } catch (const std::exception& e) {
            return crow::response(500, error_body(e.what()));
        }
    });

    CROW_ROUTE(app, "/api/alerts/<string>")
    ([store](const std::string& alert_id_text) {
        std::optional<int64_t> alert_id = parse_id(alert_id_text);
        if (!alert_id.has_value()) return crow::response(400, error_body("alert id must be an integer"));
        try {
            std::optional<tse::db::StoredAlert> found = store->get_alert(*alert_id);
            if (!found.has_value()) return crow::response(404, error_body("no alert with that id"));
            return crow::response(200, encode_alert(*found));
        } catch (const std::exception& e) {
            return crow::response(500, error_body(e.what()));
        }
    });

    // PATCH /api/alerts/<id>/status  body: {"status": "UNDER_REVIEW"}
    // The compliance-action endpoint the dashboard's action buttons call.
    // Valid values are enforced by schema.sql's CHECK constraint, not
    // re-validated here -- see AlertStore::update_alert_status()'s own
    // comment on why duplicating that list in a second place isn't done.
    CROW_ROUTE(app, "/api/alerts/<string>/status")
        .methods("PATCH"_method)([store](const crow::request& req, const std::string& alert_id_text) {
            std::optional<int64_t> alert_id = parse_id(alert_id_text);
            if (!alert_id.has_value()) return crow::response(400, error_body("alert id must be an integer"));

            crow::json::rvalue body = crow::json::load(req.body);
            if (!body || !body.has("status")) {
                return crow::response(400, error_body("request body must be {\"status\": \"...\"}"));
            }
            const std::string new_status = std::string(body["status"]);

            try {
                store->update_alert_status(*alert_id, new_status);
            } catch (const std::exception& e) {
                return crow::response(400, error_body(e.what()));
            }
            std::optional<tse::db::StoredAlert> updated = store->get_alert(*alert_id);
            return crow::response(200, encode_alert(*updated));
        });

    CROW_ROUTE(app, "/api/orderbook/<string>/snapshot")
    ([book_registry](const std::string& instrument_id) {
        if (book_registry == nullptr) {
            return crow::response(503, error_body("no live pipeline is attached to this server"));
        }
        std::optional<tse::orderbook::DepthSnapshot> snapshot = book_registry->snapshot(instrument_id);
        if (!snapshot.has_value()) {
            return crow::response(404, error_body("no book state for that instrument yet"));
        }
        return crow::response(200, encode_depth_snapshot(*snapshot));
    });

    // GET /api/orderbook/<instrument>/events[?limit=N] -- BOOK's FIX
    // message feed. Unlike the snapshot route above, an instrument that's
    // never traded isn't an error case here: a feed's natural empty state
    // is an empty list, not a 404 (there's no "book exists or doesn't"
    // distinction the way there is for order-book state).
    CROW_ROUTE(app, "/api/orderbook/<string>/events")
    ([book_registry](const crow::request& req, const std::string& instrument_id) {
        if (book_registry == nullptr) {
            return crow::response(503, error_body("no live pipeline is attached to this server"));
        }
        const char* limit_param = req.url_params.get("limit");
        size_t limit = limit_param != nullptr ? static_cast<size_t>(std::stoul(limit_param)) : 50;
        return crow::response(200, encode_book_events(book_registry->recent_events(instrument_id, limit)));
    });

    // GET /api/evaluation -- EVALUATION tab's entire data source. Reads
    // three committed, harness-generated JSON files off disk and merges
    // them verbatim; never computes anything itself (an 11-point rate
    // sweep costs real minutes against a real Kafka broker -- far too
    // expensive to run per HTTP request). 503 if any file is missing,
    // naming exactly which one and how to regenerate it, rather than
    // silently serving partial or fabricated data.
    CROW_ROUTE(app, "/api/evaluation")
    ([evaluation_results_dir] {
        struct Snapshot {
            std::string filename;
            std::string regen_hint;
        };
        const std::vector<Snapshot> snapshots = {
            {"evaluation.json", "run ./build-bench/cpp/harness/tse_harness_eval --json " + evaluation_results_dir + "/evaluation.json"},
            {"sarao.json", "run ./build-bench/cpp/harness/tse_sarao_validation --json " + evaluation_results_dir + "/sarao.json"},
            {"spoofing_rate_sweep.json", "run ./cpp/harness/run_rate_sweep.sh"},
        };

        crow::json::wvalue out;
        for (const auto& snapshot : snapshots) {
            const std::string path = evaluation_results_dir + "/" + snapshot.filename;
            std::optional<std::string> text = read_file(path);
            if (!text.has_value()) {
                return crow::response(503, error_body("missing evaluation snapshot " + path + " -- " + snapshot.regen_hint));
            }
            crow::json::rvalue parsed = crow::json::load(*text);
            if (!parsed) {
                return crow::response(500, error_body(path + " exists but failed to parse as JSON"));
            }
            const std::string key = snapshot.filename.substr(0, snapshot.filename.size() - 5);  // strip ".json"
            out[key] = crow::json::wvalue(parsed);
        }
        return crow::response(200, out);
    });
}

}  // namespace tse::api
