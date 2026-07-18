#include "ground_truth.hpp"

namespace tse::harness {

const tse::simulator::GroundTruthLabel* GroundTruthIndex::lookup_order(const std::string& order_id) const {
    auto it = label_by_order_id.find(order_id);
    return it == label_by_order_id.end() ? nullptr : &it->second;
}

const tse::simulator::GroundTruthLabel* GroundTruthIndex::lookup_trade(const std::string& trade_id) const {
    auto it = label_by_trade_id.find(trade_id);
    return it == label_by_trade_id.end() ? nullptr : &it->second;
}

GroundTruthIndex build_ground_truth_index(const tse::simulator::SimulationOutput& simulation) {
    GroundTruthIndex index;
    for (const auto& order : simulation.orders) {
        // A New and its later Cancel share order_id and carry the same
        // label (see abuse/spoofing_layering.cpp) -- last-write-wins here
        // is harmless since both writes agree.
        index.label_by_order_id[order.order_id] = order.ground_truth_label;
    }
    for (const auto& execution : simulation.executions) {
        index.label_by_trade_id[execution.trade_id] = execution.ground_truth_label;
    }
    return index;
}

}  // namespace tse::harness
