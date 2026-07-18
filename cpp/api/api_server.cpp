#include "api_server.hpp"

#include <exception>
#include <optional>
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

}  // namespace

void register_routes(App& app, tse::db::AlertStore* store, LiveBookRegistry* book_registry) {
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

    // GET /api/alerts[?account_id=...|detector_name=...|start_ns=...&end_ns=...][&limit=N]
    // The three query params map directly onto AlertStore's three Phase 8
    // query methods -- one filter per request, not a combinator, mirroring
    // exactly the query shapes that layer exposes. No filter given at all
    // falls back to list_recent_alerts(), the "what does a compliance
    // analyst see when they first open the dashboard" case.
    CROW_ROUTE(app, "/api/alerts")
    ([store](const crow::request& req) {
        try {
            const char* account_id = req.url_params.get("account_id");
            const char* detector_name = req.url_params.get("detector_name");
            const char* start_ns_param = req.url_params.get("start_ns");
            const char* end_ns_param = req.url_params.get("end_ns");
            const char* limit_param = req.url_params.get("limit");

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
}

}  // namespace tse::api
