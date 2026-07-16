#pragma once

#include <string>
#include <vector>

#include "types.hpp"

namespace tse::simulator {

// Both include ground_truth_label columns — evaluation/replay-mode output
// only (feeds Phase 10's harness).
std::string orders_to_csv(const std::vector<Order>& orders);
std::string executions_to_csv(const std::vector<Execution>& executions);

// Inverse of the above — parses CSV in the exact column layout this
// module's writers emit (including the gt_* ground-truth columns) back into
// structs. Used to prove round-trip recoverability.
std::vector<Order> parse_orders_csv(const std::string& csv);
std::vector<Execution> parse_executions_csv(const std::string& csv);

}  // namespace tse::simulator
