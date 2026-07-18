#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tse::detectors {

// What a detector emits when it fires. Mirrors the architecture doc's
// "Alert evidence contract" (detector name, order/trade IDs involved, a
// book depth snapshot reference, and the time window) and the HLD's Alert
// entity (alert_id, detector_type, score/severity, account_id(s),
// instrument_id, evidence, status, ground_truth_label).
//
// Deliberately omits alert_id (assigned at persistence time — Phase 8's
// concern, not a detector's) and status (case-management state — Phase 9's
// concern). Deliberately and structurally omits ground_truth_label: this
// struct is what a live detector running against live book state produces;
// per CLAUDE.md, that field must never appear in a live-mode code path, so
// it isn't a field a detector could even populate, mirroring how
// fix::Order/Execution (the live-mode structs) have no such field while
// their simulator:: counterparts do.
//
// `score` is a continuous value, not a boolean fire/no-fire flag — this is
// what makes Phase 10's "threshold sweep + confusion matrix per detector"
// possible later without re-running detection logic per threshold. By
// convention scores fall in [0, 1]; WashTradeDetector (a deterministic
// rule, not a heuristic) always emits 1.0 when it fires. Each detector
// applies its own configurable threshold internally and only returns an
// Alert once score clears it — evaluate() returning an empty vector means
// "nothing met this detector's own bar," not "score was exactly zero."
struct Alert {
    std::string detector_name;
    double score{0.0};
    std::string instrument_id;
    std::vector<std::string> account_ids;  // one or more accounts implicated
    std::vector<std::string> order_ids;    // order/trade IDs that constitute the evidence
    int64_t window_start_ns{0};
    int64_t window_end_ns{0};
    std::string evidence;  // human-readable explanation of why this fired

    // Populated only by model-backed detectors (currently just
    // MlAnomalyDetector) so Phase 8's persistence layer can store and query
    // it directly instead of parsing it back out of `evidence`. Empty for
    // every deterministic-rule detector, which has no model version to
    // report -- unset, not a sentinel string, is what "not applicable" looks
    // like here.
    std::optional<std::string> model_version;

    // OrderBook::sequence() at the moment this Alert was constructed -- the
    // architecture doc's "book depth snapshot reference." Currently just a
    // stored reference: nothing in this codebase reads it back to
    // reconstruct a DepthSnapshot yet, and no phase currently commits to
    // building that (cpp/harness/, the natural home for it, is still an
    // unbuilt Phase 10 scaffold whose documented scope is precision/recall/
    // F1/threshold-sweep, not sequence-indexed book reconstruction). The
    // event-sourced, deterministically-replayable design (ingestion/'s
    // durable Kafka log, pipeline/'s single reusable live/replay code path)
    // means reconstruction is buildable later without a schema change --
    // replaying this instrument's event stream up to this sequence number
    // would reproduce the exact book state a detector was looking at -- but
    // that capability doesn't exist today, only the reference value does.
    //
    // Populated by every firing detector, including MlAnomalyDetector
    // (whose evaluate() captures book.sequence() at submission time and
    // threads it through ScoringRequest, since the OrderBook instance
    // itself isn't available on MlScoringWorker's later, separate thread —
    // see ml_anomaly_detector.hpp). std::optional rather than a plain
    // int64_t only because a hand-constructed Alert in a test has no
    // natural default sequence value to assume.
    std::optional<int64_t> book_snapshot_sequence;
};

}  // namespace tse::detectors
