#pragma once

#include <vector>

#include <crow/json.h>

#include "depth_snapshot.hpp"
#include "stored_alert.hpp"

namespace tse::api {

// Uses crow::json::wvalue (already part of the Crow dependency this module
// brings in) rather than a fourth hand-rolled JSON codec -- unlike
// ml_client/simulator (raw sockets/files, no HTTP framework already
// providing one), a JSON builder is already sitting right here as part of
// Crow itself, so hand-rolling a duplicate would be adding code, not
// avoiding a dependency.
crow::json::wvalue encode_alert(const tse::db::StoredAlert& stored);
crow::json::wvalue encode_alerts(const std::vector<tse::db::StoredAlert>& alerts);
crow::json::wvalue encode_depth_snapshot(const tse::orderbook::DepthSnapshot& snapshot);

}  // namespace tse::api
