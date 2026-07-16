#pragma once

#include <string>
#include <vector>

#include "types.hpp"

namespace tse::simulator {

// Includes ground_truth_label for every event — evaluation/replay-mode
// output only (feeds Phase 10's harness). Per CLAUDE.md's data-compliance
// rules, never wire this into anything that looks like a live-mode path.
std::string to_labeled_json(const std::vector<Order>& orders, const std::vector<Execution>& executions);

struct ParsedEvents {
    std::vector<Order> orders;
    std::vector<Execution> executions;
};

// Parses JSON in the exact shape to_labeled_json produces back into
// Order/Execution structs, including ground_truth_label — this is what
// proves round-trip recoverability, not just that the label text is present
// somewhere in the output. Not a general-purpose JSON parser: it assumes the
// object/array/key shape this module's own writer emits.
ParsedEvents parse_labeled_json(const std::string& json);

}  // namespace tse::simulator
