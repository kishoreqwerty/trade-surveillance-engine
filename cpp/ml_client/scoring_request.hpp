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

    // OrderBook::sequence() at the moment MlAnomalyDetector::evaluate()
    // submitted this request -- carried through so that if this request
    // ends up producing an Alert (on MlScoringWorker's later, separate
    // thread, with no OrderBook reference of its own), that Alert can still
    // populate Alert::book_snapshot_sequence. Not part of the wire payload
    // to ml_service/ (see ml_json.cpp's encode_scoring_request) -- purely
    // local bookkeeping between MlAnomalyDetector and MlScoringWorker.
    int64_t book_snapshot_sequence{0};
};

// { "anomaly_score": float, "model_version": string } — the response side
// of the same contract.
struct ScoringResult {
    double anomaly_score{0.0};
    std::string model_version;
};

}  // namespace tse::ml_client
