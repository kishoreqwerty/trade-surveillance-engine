#pragma once

#include <string>

#include <crow.h>
#include <crow/middlewares/cors.h>

#include "alert_store.hpp"
#include "live_book_registry.hpp"
#include "ml_score_client.hpp"

namespace tse::api {

// The app type every server/test in this project uses -- CORS enabled
// (see register_routes()) so a browser-based dashboard served from a
// different origin (Vite's dev server on :5173, the API on :8081) can
// actually call it. Found necessary by real browser verification, not
// assumed: curl doesn't enforce CORS, so the route-level tests
// (cpp/tests/api/) all passed before this was added -- only driving the
// actual dashboard in a real headless Chromium surfaced the blocked
// fetch(). A plain crow::SimpleApp is what caused that gap.
using App = crow::App<crow::CORSHandler>;

// Registers every Phase 9 REST route onto `app`, plus a permissive CORS
// policy (this is a local, unauthenticated dev API -- same posture this
// project already takes with Kafka/TimescaleDB, see docker-compose.yml).
// book_registry is nullable: GET /api/orderbook/<instrument>/snapshot
// returns 503 rather than dereferencing a null pointer when no live FIX
// session is standing behind this process (e.g. cpp/tests/api/'s
// route-level tests, which only exercise the alert-store-backed routes
// against a real TimescaleDB, not a full live pipeline).
//
// ml_config configures GET /api/status's ML-service health check. A
// fresh tse::ml_client::MlScoreClient is constructed per request from
// this config rather than sharing one client across Crow's handler
// threads and MlScoringWorker's own thread -- httplib::Client isn't
// documented safe for that kind of concurrent cross-thread use, and nothing
// about a low-frequency status poll needs the cost of a shared client with
// its own locking to avoid that risk.
// evaluation_results_dir points at the committed, harness-generated JSON
// snapshots GET /api/evaluation serves (cpp/harness/results/ by default --
// regenerate via tse_harness_eval --json / tse_sarao_validation --json /
// run_rate_sweep.sh, never hand-edited). Relative paths resolve against
// this process's working directory, matching every other manual-run
// convention in this project (docker compose, the demo server itself).
void register_routes(App& app, tse::db::AlertStore* store, LiveBookRegistry* book_registry,
                      tse::ml_client::MlScoreClientConfig ml_config = {},
                      std::string evaluation_results_dir = "cpp/harness/results");

}  // namespace tse::api
