#pragma once

#include <optional>
#include <string>

#include "scoring_request.hpp"

namespace tse::ml_client {

// Hand-rolled, scoped JSON handling — matching this project's established
// convention (cpp/simulator/serialization/json_writer.cpp,
// cpp/ingestion/event_codec.cpp) rather than pulling in a general-purpose
// JSON library for two fixed, simple shapes.

// Encodes exactly the architecture doc's request shape:
// {"account_id":"...","instrument_id":"...","window_features":{"k":v,...}}
std::string encode_scoring_request(const ScoringRequest& request);

// Decodes exactly {"anomaly_score": <number>, "model_version": "<string>"}.
// Returns std::nullopt if either field is missing or malformed —
// deliberately tolerant of whitespace/key order within that shape, but
// not a general JSON parser; this only ever needs to read the one
// response shape ml_service/ produces.
std::optional<ScoringResult> decode_scoring_response(const std::string& body);

}  // namespace tse::ml_client
