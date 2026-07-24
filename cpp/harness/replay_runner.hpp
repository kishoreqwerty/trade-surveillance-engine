#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "alert.hpp"
#include "ml_score_client.hpp"
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

// Opt-in: pass a pointer to include MlAnomalyDetector in the replay (see
// replay_through_kafka's ml_eval parameter). Originally excluded outright
// (that reasoning -- wall-clock nondeterminism from an out-of-band HTTP
// round trip -- is now handled structurally instead, not sidestepped: see
// replay_through_kafka's implementation, which only signals
// MlScoringWorker to stop after the consumer thread has fully finished
// submitting, then joins it, so the result never depends on how long any
// individual score() call took). This gap was unvalidated from Phase 7
// (when MlAnomalyDetector was built) until a live-dashboard observation in
// Phase 12 -- see cpp/harness/README.md.
struct MlEvalConfig {
    // base_url defaults to ml_service's own default (127.0.0.1:8000).
    tse::ml_client::MlScoreClientConfig client{};
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
// ml_eval: nullptr (default) reproduces the original five-detector-only
// behavior exactly -- every existing caller is unaffected. Non-null adds a
// sixth detector, MlAnomalyDetector, wired to a real MlScoringWorker
// against a real (must be running) ml_service, on its own thread exactly
// the way cpp/api/main.cpp's live wiring does. Fails loud via
// MlScoreClient::health_check() before the replay starts if ml_service
// isn't reachable, rather than letting every request silently fail open
// (MlScoringWorker's normal, correct production behavior) and produce a
// misleadingly empty result. The worker's alert_threshold is fixed at 0.0
// for this path regardless of production's 0.7 default -- evaluation needs
// every scored window's raw score to sweep thresholds after the fact
// (see evaluation.hpp's ml_anomaly_threshold_sweep), exactly like the five
// rule-based detectors already work.
//
// brokers: e.g. "localhost:9092". topic: must be unique per call (caller's
// responsibility -- mirrors cpp/tests/ingestion/kafka_replay_test.cpp's
// topic_name() pattern) since this seeks to the beginning of the topic and
// a reused topic name would replay stale data from a previous run.
//
// Throws std::runtime_error if the Kafka broker is unreachable, if
// ml_eval is non-null and ml_service isn't reachable, if the full
// published event count isn't recovered by replay, or if either the main
// SPSC ring buffer or (when ml_eval is set) the ML scoring queue's
// drop-oldest policy discarded anything -- an evaluation run's precision/
// recall numbers are meaningless if the replay wasn't complete, so this
// fails loud rather than silently returning a partial result.
ReplayResult replay_through_kafka(const tse::simulator::SimulationOutput& simulation, const std::string& brokers,
                                   const std::string& topic, int publish_timeout_ms = 20000,
                                   int poll_timeout_ms = 2000, const MlEvalConfig* ml_eval = nullptr);

}  // namespace tse::harness
