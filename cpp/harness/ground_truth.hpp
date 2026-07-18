#pragma once

#include <string>
#include <unordered_map>

#include "simulator.hpp"

namespace tse::harness {

// Evaluation-only index from event id -> the ground-truth label that
// produced it, built directly from Phase 1's SimulationOutput before
// translation to the live-mode fix::Order/Execution structs (which have no
// such field -- see cpp/fix/types.hpp's header comment). Nothing on the
// replay path (replay_runner.hpp) ever holds a reference to this index --
// it exists purely so evaluation.hpp can score fired Alerts *after* the
// fact, keeping ground_truth_label structurally out of the live-mode code
// path per CLAUDE.md's data rules.
struct GroundTruthIndex {
    std::unordered_map<std::string, tse::simulator::GroundTruthLabel> label_by_order_id;
    std::unordered_map<std::string, tse::simulator::GroundTruthLabel> label_by_trade_id;

    // nullptr if the id isn't known (e.g. a synthetic counterparty id, or
    // an id from a different simulation run).
    const tse::simulator::GroundTruthLabel* lookup_order(const std::string& order_id) const;
    const tse::simulator::GroundTruthLabel* lookup_trade(const std::string& trade_id) const;
};

GroundTruthIndex build_ground_truth_index(const tse::simulator::SimulationOutput& simulation);

}  // namespace tse::harness
