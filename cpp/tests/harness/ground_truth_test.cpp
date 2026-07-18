#include "ground_truth.hpp"

#include <gtest/gtest.h>

using tse::simulator::AbusePattern;
using tse::simulator::Execution;
using tse::simulator::GroundTruthLabel;
using tse::simulator::Order;
using tse::simulator::SimulationOutput;

TEST(GroundTruthIndexTest, IndexesOrdersAndExecutionsSeparately) {
    SimulationOutput simulation;

    Order order;
    order.order_id = "ORD-1";
    order.ground_truth_label = GroundTruthLabel{AbusePattern::kSpoofingLayering, "SCN-SPOOF-001", 0.7};
    simulation.orders.push_back(order);

    Execution execution;
    execution.trade_id = "EXE-1";
    execution.ground_truth_label = GroundTruthLabel{AbusePattern::kWashTrade, "SCN-WASH-001", 0.4};
    simulation.executions.push_back(execution);

    tse::harness::GroundTruthIndex index = tse::harness::build_ground_truth_index(simulation);

    const auto* order_label = index.lookup_order("ORD-1");
    ASSERT_NE(order_label, nullptr);
    EXPECT_EQ(order_label->pattern, AbusePattern::kSpoofingLayering);
    EXPECT_EQ(order_label->scenario_id, "SCN-SPOOF-001");
    EXPECT_DOUBLE_EQ(order_label->severity, 0.7);

    const auto* trade_label = index.lookup_trade("EXE-1");
    ASSERT_NE(trade_label, nullptr);
    EXPECT_EQ(trade_label->pattern, AbusePattern::kWashTrade);

    // An order_id is never found via lookup_trade and vice versa -- the two
    // namespaces (ORD-/EXE-) are kept genuinely separate.
    EXPECT_EQ(index.lookup_trade("ORD-1"), nullptr);
    EXPECT_EQ(index.lookup_order("EXE-1"), nullptr);
}

TEST(GroundTruthIndexTest, UnknownIdLooksUpAsNullptr) {
    tse::harness::GroundTruthIndex index = tse::harness::build_ground_truth_index(SimulationOutput{});
    EXPECT_EQ(index.lookup_order("MKT-COUNTERPARTY"), nullptr);
    EXPECT_EQ(index.lookup_trade("NONEXISTENT"), nullptr);
}
