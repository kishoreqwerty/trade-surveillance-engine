#include <gtest/gtest.h>

#include "types.hpp"

using namespace tse::simulator;

TEST(TypesGroundTruth, DefaultLabelIsBaseline) {
    Order order;
    EXPECT_EQ(order.ground_truth_label.pattern, AbusePattern::kBaseline);
    EXPECT_FALSE(is_abuse(order.ground_truth_label));

    Execution execution;
    EXPECT_EQ(execution.ground_truth_label.pattern, AbusePattern::kBaseline);
    EXPECT_FALSE(is_abuse(execution.ground_truth_label));
}

TEST(TypesGroundTruth, StripLabelResetsToBaseline) {
    Order order;
    order.order_id = "ORD-1";
    order.price = 101.5;
    order.ground_truth_label = {AbusePattern::kWashTrade, "SCN-WASH-000001", 0.8};
    ASSERT_TRUE(is_abuse(order.ground_truth_label));

    Order stripped = strip_label(order);
    EXPECT_FALSE(is_abuse(stripped.ground_truth_label));
    EXPECT_EQ(stripped.ground_truth_label.scenario_id, "");
    // Everything else about the order is preserved.
    EXPECT_EQ(stripped.order_id, order.order_id);
    EXPECT_EQ(stripped.price, order.price);
}

TEST(TypesGroundTruth, StripLabelWorksForExecutionsToo) {
    Execution execution;
    execution.trade_id = "EXE-1";
    execution.ground_truth_label = {AbusePattern::kFrontRunning, "SCN-FR-000002", 0.3};

    Execution stripped = strip_label(execution);
    EXPECT_FALSE(is_abuse(stripped.ground_truth_label));
    EXPECT_EQ(stripped.trade_id, execution.trade_id);
}

TEST(TypesGroundTruth, ToStringCoversAllAbusePatterns) {
    EXPECT_STREQ(to_string(AbusePattern::kBaseline), "Baseline");
    EXPECT_STREQ(to_string(AbusePattern::kWashTrade), "WashTrade");
    EXPECT_STREQ(to_string(AbusePattern::kSpoofingLayering), "SpoofingLayering");
    EXPECT_STREQ(to_string(AbusePattern::kMarkingTheClose), "MarkingTheClose");
    EXPECT_STREQ(to_string(AbusePattern::kFrontRunning), "FrontRunning");
}
