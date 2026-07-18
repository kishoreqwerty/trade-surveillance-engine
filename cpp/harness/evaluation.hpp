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

}  // namespace tse::harness
