#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "alert.hpp"
#include "simulator.hpp"

namespace tse::harness {

struct ReplayResult {
    std::vector<tse::detectors::Alert> alerts;
    uint64_t events_total{0};
    uint64_t events_replayed_from_kafka{0};
    uint64_t events_processed{0};
    uint64_t events_skipped_inconsistent{0};
    uint64_t ring_buffer_dropped{0};
};

// Runs one Phase 1 SimulationOutput through Kafka (publish, then
// seek-to-beginning replay -- see cpp/ingestion/kafka_consumer.hpp) into a
// fresh LivePipeline wired with the five deterministic/rule-based
// detectors from Phase 5, popped by an unmodified LiveConsumer exactly the
// way cpp/api/main.cpp's production wiring does (see live_consumer.hpp's
// own header comment: "Phase 10's replay harness is expected to reuse this
// same class from its own driver loop"). Not a separate offline scoring
// path -- this *is* pipeline/'s LivePipeline, given real (stripped of
// ground truth) fix::Order/Execution events recovered from a real Kafka
// topic.
//
// Deliberately excludes MlAnomalyDetector: that detector requires
// ml_service/ to be running and calls it asynchronously (see
// cpp/ml_client/README.md), which would make this evaluation's alert
// output depend on wall-clock timing of an out-of-band HTTP round trip --
// exactly the nondeterminism KafkaReplayConsumer's seek_to_beginning
// design was built to avoid at the ingestion layer (see
// cpp/ingestion/kafka_consumer.hpp's header comment). Phase 10's own scope
// ("precision/recall/F1 per detector ... vs. StatisticalBaselineDetector")
// only ever names the five Phase 5 detectors.
//
// brokers: e.g. "localhost:9092". topic: must be unique per call (caller's
// responsibility -- mirrors cpp/tests/ingestion/kafka_replay_test.cpp's
// topic_name() pattern) since this seeks to the beginning of the topic and
// a reused topic name would replay stale data from a previous run.
//
// Throws std::runtime_error if the Kafka broker is unreachable, if the
// full published event count isn't recovered by replay, or if the SPSC
// ring buffer's drop-oldest policy discarded anything (see
// cpp/ingestion/spsc_ring_buffer.hpp) -- an evaluation run's precision/
// recall numbers are meaningless if the replay wasn't complete, so this
// fails loud rather than silently returning a partial result.
ReplayResult replay_through_kafka(const tse::simulator::SimulationOutput& simulation, const std::string& brokers,
                                   const std::string& topic, int publish_timeout_ms = 20000,
                                   int poll_timeout_ms = 2000);

}  // namespace tse::harness
