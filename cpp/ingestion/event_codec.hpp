#pragma once

#include <string>

#include "ingestion_event.hpp"

namespace tse::ingestion {

// Serializes an IngestionEvent to the exact bytes written as a Kafka
// message payload (see kafka_producer.hpp) — this is what durability and
// replay determinism actually rest on, so it round-trips every field
// exactly, including full double precision and exact int64 timestamps (see
// cpp/simulator/serialization/json_writer.cpp's header comment for why a
// naive JSON number round-trip silently loses both of those).
std::string encode(const IngestionEvent& event);

// Inverse of encode(). Throws std::runtime_error on malformed input.
IngestionEvent decode(const std::string& json);

}  // namespace tse::ingestion
