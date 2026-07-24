#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "alert.hpp"
#include "simulator.hpp"

namespace tse::harness {

// tp/fp/fn/tn are all counted in the same unit: event ids (order_id or
// trade_id, depending on the detector -- see DetectorUniverse) drawn from
// one detector's own evidence universe. precision()/recall()/f1() return
// NaN (not 0.0 or 1.0) when their denominator is zero -- "this detector
// never fired at this threshold" and "this detector fired perfectly" are
// different facts, and silently picking a sentinel value would blur them.
// report.cpp is responsible for rendering NaN as "n/a", not this struct.
struct ConfusionMatrix {
    uint64_t tp{0};
    uint64_t fp{0};
    uint64_t fn{0};
    uint64_t tn{0};

    double precision() const;
    double recall() const;
    double f1() const;
};

// One detector's fixed evidence-universe: the set of every id of the event
// type that detector reacts to (see each detector's .cpp dispatch for
// which DetectorEvent arm / OrderStatus it matches on), and that same set
// partitioned by AbusePattern via each record's own ground_truth_label.
// This is evaluation-only bookkeeping built from Phase 1's SimulationOutput
// directly -- it never touches replay_runner.cpp's live replay path.
struct DetectorUniverse {
    std::unordered_set<std::string> event_ids;
    std::unordered_map<tse::simulator::AbusePattern, std::unordered_set<std::string>> ids_by_pattern;
};

struct Universes {
    DetectorUniverse new_orders;    // order_id of every OrderStatus::kNew record
    DetectorUniverse cancel_orders; // order_id of every OrderStatus::kCancelled record
    DetectorUniverse executions;    // trade_id of every Execution record
};

Universes build_universes(const tse::simulator::SimulationOutput& simulation);

// ids_by_pattern[target_pattern], or an empty set if that pattern never
// occurs in this universe (e.g. threshold sweep run against a simulation
// with zero scenarios of that pattern).
const std::unordered_set<std::string>& positive_ids_for(const DetectorUniverse& universe,
                                                          tse::simulator::AbusePattern target_pattern);

// Union of ids_by_pattern across every pattern except kBaseline -- "did
// this order/execution belong to ANY injected abuse scenario," used for
// StatisticalBaselineDetector's pattern-agnostic aggregate comparison.
std::unordered_set<std::string> any_abuse_ids(const DetectorUniverse& universe);

// predicted-positive = union of Alert::order_ids across every Alert with
// detector_name == detector_name and score >= score_threshold, intersected
// with universe_ids (an id outside this universe -- e.g. the order_id half
// of a WashTradeDetector alert's order_ids, {execution->order_id,
// execution->trade_id}, when universe_ids is the trade_id universe -- is
// silently ignored, not miscounted as a false positive).
ConfusionMatrix compute_confusion_matrix(const std::vector<tse::detectors::Alert>& alerts,
                                          const std::string& detector_name, double score_threshold,
                                          const std::unordered_set<std::string>& universe_ids,
                                          const std::unordered_set<std::string>& positive_ids);

struct SweepPoint {
    double threshold{0.0};
    ConfusionMatrix matrix;
};

std::vector<SweepPoint> threshold_sweep(const std::vector<tse::detectors::Alert>& alerts,
                                         const std::string& detector_name,
                                         const std::unordered_set<std::string>& universe_ids,
                                         const std::unordered_set<std::string>& positive_ids,
                                         const std::vector<double>& thresholds);

// One pattern-aware detector's fixed target pattern + which universe its
// evidence ids are drawn from -- mirrors the mapping documented in each
// detector's own class comment (front_running_detector.hpp,
// spoofing_layering_detector.hpp, etc.), not a re-derivation of it.
struct DetectorSpec {
    std::string detector_name;
    tse::simulator::AbusePattern target_pattern;
};

// WashTradeDetector -> kExecution universe, SpoofingLayeringDetector ->
// kCancelOrder (it only ever fires from handle_cancel()), MarkingTheClose
// -> kExecution, FrontRunning -> kNewOrder. Caller picks the matching
// DetectorUniverse out of a Universes via detector_universe_kind().
const std::vector<DetectorSpec>& pattern_aware_detector_specs();

enum class UniverseKind { kNewOrder, kCancelOrder, kExecution };
UniverseKind detector_universe_kind(const std::string& detector_name);
const DetectorUniverse& select_universe(const Universes& universes, UniverseKind kind);

// MlAnomalyDetector's evidence is a rolling (account_id, instrument_id)
// window statistic, not a claim about any specific order/execution -- its
// Alerts carry no order_ids at all (see ml_scoring_worker.cpp's
// process_one(), which only sets account_ids/instrument_id/window bounds),
// so compute_confusion_matrix's order-id intersection can't score it.
// This is the detector's native granularity instead: built once from
// SimulationOutput, keyed by "account_id|instrument_id", holding every
// non-baseline order timestamp for that key (MlAnomalyDetector only ever
// derives its features from Order events -- see ml_anomaly_detector.cpp's
// handle_order() -- so execution timestamps are deliberately not
// included here).
struct MlAnomalyGroundTruth {
    std::unordered_map<std::string, std::vector<int64_t>> abuse_order_timestamps_by_key;
};

MlAnomalyGroundTruth build_ml_anomaly_ground_truth(const tse::simulator::SimulationOutput& simulation);

// True if `alert`'s scored window genuinely overlapped injected abuse
// activity for the same (account, instrument) key. MlAnomalyDetector's
// Alert only carries a single instant (window_start_ns == window_end_ns ==
// the triggering order's own timestamp -- see ml_scoring_worker.cpp), not
// the tumbling window's true start, because ScoringRequest never threads
// that through. This reconstructs a safe over-approximation of the real
// window instead: [triggering_ts - window_duration_ns, triggering_ts] is
// guaranteed to contain the true window (a fresh tumbling window only ever
// resets once the gap from its start exceeds window_duration_ns -- see
// MlAnomalyDetector::handle_order()), so this can only ever call a window
// positive that a tighter reconstruction might have called negative, never
// the other way around.
bool ml_anomaly_window_is_positive(const MlAnomalyGroundTruth& ground_truth, const tse::detectors::Alert& alert,
                                    int64_t window_duration_ns);

// Same tp/fp/fn/tn shape as ConfusionMatrix, but the "universe" here is
// implicit: every Alert with detector_name == "MlAnomalyDetector" already
// represents one scored window (not an id needing a lookup), so there's no
// separate universe_ids/positive_ids pair to pass in -- see
// replay_runner.hpp's MlEvalConfig for why this only works when the
// replay's MlScoringWorker was configured with alert_threshold == 0.0
// (every scored window becomes an Alert here, not just ones that already
// cleared some earlier gate) -- without that, this would silently undercount
// FN/TN for any candidate threshold below whatever gate was applied first.
ConfusionMatrix compute_ml_anomaly_confusion_matrix(const std::vector<tse::detectors::Alert>& alerts,
                                                      double score_threshold, const MlAnomalyGroundTruth& ground_truth,
                                                      int64_t window_duration_ns);

std::vector<SweepPoint> ml_anomaly_threshold_sweep(const std::vector<tse::detectors::Alert>& alerts,
                                                     const MlAnomalyGroundTruth& ground_truth,
                                                     int64_t window_duration_ns, const std::vector<double>& thresholds);

}  // namespace tse::harness
