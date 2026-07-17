#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tse::detectors {

// What a detector emits when it fires. Mirrors the architecture doc's
// "Alert evidence contract" (detector name, order/trade IDs involved, a
// book depth snapshot reference — carried here as the window/account/order
// identifiers a Phase 8 persistence layer or Phase 9 dashboard would need
// to reconstruct *why* this fired without re-running the pipeline) and the
// HLD's Alert entity (alert_id, detector_type, score/severity, account_id(s),
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
};

}  // namespace tse::detectors
