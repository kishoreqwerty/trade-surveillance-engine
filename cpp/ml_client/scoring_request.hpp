#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace tse::ml_client {

// What crosses the wire to POST /score, per the architecture doc's ML
// microservice contract (§3): { "account_id", "instrument_id",
// "window_features": {...} }. timestamp_ns is carried alongside for the
// resulting Alert's window fields (see ml_anomaly_detector.hpp) — it is
// not itself part of the wire payload.
struct ScoringRequest {
    std::string account_id;
    std::string instrument_id;
    std::unordered_map<std::string, double> window_features;
    int64_t timestamp_ns{0};
};

// { "anomaly_score": float, "model_version": string } — the response side
// of the same contract.
struct ScoringResult {
    double anomaly_score{0.0};
    std::string model_version;
};

}  // namespace tse::ml_client
