#include "evaluation.hpp"

#include <cmath>
#include <stdexcept>

#include <gtest/gtest.h>

using tse::detectors::Alert;
using tse::harness::ConfusionMatrix;
using tse::simulator::AbusePattern;
using tse::simulator::Account;
using tse::simulator::Execution;
using tse::simulator::GroundTruthLabel;
using tse::simulator::Instrument;
using tse::simulator::Order;
using tse::simulator::OrderStatus;
using tse::simulator::SimulationOutput;

namespace {

Order make_new_order(const std::string& id, AbusePattern pattern, const std::string& scenario_id = "SCN-1") {
    Order order;
    order.order_id = id;
    order.status = OrderStatus::kNew;
    order.ground_truth_label = GroundTruthLabel{pattern, scenario_id, 0.5};
    return order;
}

Order make_cancel_order(const std::string& id, AbusePattern pattern, const std::string& scenario_id = "SCN-1") {
    Order order = make_new_order(id, pattern, scenario_id);
    order.status = OrderStatus::kCancelled;
    return order;
}

Execution make_execution(const std::string& trade_id, AbusePattern pattern, const std::string& scenario_id = "SCN-1") {
    Execution execution;
    execution.trade_id = trade_id;
    execution.ground_truth_label = GroundTruthLabel{pattern, scenario_id, 0.5};
    return execution;
}

Alert make_alert(const std::string& detector_name, double score, std::vector<std::string> order_ids) {
    Alert alert;
    alert.detector_name = detector_name;
    alert.score = score;
    alert.order_ids = std::move(order_ids);
    return alert;
}

}  // namespace

TEST(EvaluationTest, ConfusionMatrixCountsAllFourOutcomes) {
    // Universe of 4 ids; 2 are ground-truth positive; the detector predicts
    // one true positive, one false positive, missing one true positive,
    // correctly leaving one true negative alone.
    std::unordered_set<std::string> universe_ids = {"A", "B", "C", "D"};
    std::unordered_set<std::string> positive_ids = {"A", "B"};
    std::vector<Alert> alerts = {
        make_alert("Detector", 1.0, {"A"}),  // TP
        make_alert("Detector", 1.0, {"C"}),  // FP
        // "B" never predicted -> FN
        // "D" never predicted, not positive -> TN
    };

    ConfusionMatrix matrix = tse::harness::compute_confusion_matrix(alerts, "Detector", 0.5, universe_ids, positive_ids);

    EXPECT_EQ(matrix.tp, 1u);
    EXPECT_EQ(matrix.fp, 1u);
    EXPECT_EQ(matrix.fn, 1u);
    EXPECT_EQ(matrix.tn, 1u);
    EXPECT_DOUBLE_EQ(matrix.precision(), 0.5);
    EXPECT_DOUBLE_EQ(matrix.recall(), 0.5);
    EXPECT_DOUBLE_EQ(matrix.f1(), 0.5);
}

TEST(EvaluationTest, IgnoresOtherDetectorsAndIdsOutsideTheUniverse) {
    std::unordered_set<std::string> universe_ids = {"A"};
    std::unordered_set<std::string> positive_ids = {"A"};
    std::vector<Alert> alerts = {
        make_alert("OtherDetector", 1.0, {"A"}),        // wrong detector -- must not count
        make_alert("Detector", 1.0, {"NOT-IN-UNIVERSE"}),  // id outside universe -- must not count as FP
    };

    ConfusionMatrix matrix = tse::harness::compute_confusion_matrix(alerts, "Detector", 0.5, universe_ids, positive_ids);

    EXPECT_EQ(matrix.tp, 0u);
    EXPECT_EQ(matrix.fp, 0u);
    EXPECT_EQ(matrix.fn, 1u);  // "A" was positive and never (correctly) predicted
    EXPECT_EQ(matrix.tn, 0u);
}

TEST(EvaluationTest, ThresholdExcludesLowerScoringAlerts) {
    std::unordered_set<std::string> universe_ids = {"A", "B"};
    std::unordered_set<std::string> positive_ids = {"A", "B"};
    std::vector<Alert> alerts = {
        make_alert("Detector", 0.3, {"A"}),
        make_alert("Detector", 0.9, {"B"}),
    };

    ConfusionMatrix low = tse::harness::compute_confusion_matrix(alerts, "Detector", 0.5, universe_ids, positive_ids);
    EXPECT_EQ(low.tp, 1u);  // only B clears 0.5
    EXPECT_EQ(low.fn, 1u);

    ConfusionMatrix all = tse::harness::compute_confusion_matrix(alerts, "Detector", 0.0, universe_ids, positive_ids);
    EXPECT_EQ(all.tp, 2u);
    EXPECT_EQ(all.fn, 0u);
}

TEST(EvaluationTest, PrecisionAndRecallAreNaNNotZeroWhenUndefined) {
    std::unordered_set<std::string> universe_ids = {"A"};
    std::unordered_set<std::string> empty_positive;
    std::vector<Alert> no_alerts;

    // No alerts fired at all -> precision undefined (0 predicted), recall
    // undefined too since there's nothing to find (0 positive).
    ConfusionMatrix matrix = tse::harness::compute_confusion_matrix(no_alerts, "Detector", 0.5, universe_ids, empty_positive);
    EXPECT_TRUE(std::isnan(matrix.precision()));
    EXPECT_TRUE(std::isnan(matrix.recall()));
    EXPECT_TRUE(std::isnan(matrix.f1()));
}

TEST(EvaluationTest, ThresholdSweepIsMonotonicNonIncreasingInPredictedPositives) {
    std::unordered_set<std::string> universe_ids = {"A", "B", "C"};
    std::unordered_set<std::string> positive_ids = {"A", "B", "C"};
    std::vector<Alert> alerts = {
        make_alert("Detector", 0.2, {"A"}),
        make_alert("Detector", 0.5, {"B"}),
        make_alert("Detector", 0.9, {"C"}),
    };

    auto sweep = tse::harness::threshold_sweep(alerts, "Detector", universe_ids, positive_ids, {0.0, 0.3, 0.6, 1.0});

    ASSERT_EQ(sweep.size(), 4u);
    EXPECT_EQ(sweep[0].matrix.tp, 3u);  // threshold 0.0: all three clear
    EXPECT_EQ(sweep[1].matrix.tp, 2u);  // threshold 0.3: drops the 0.2 one
    EXPECT_EQ(sweep[2].matrix.tp, 1u);  // threshold 0.6: only the 0.9 one
    EXPECT_EQ(sweep[3].matrix.tp, 0u);  // threshold 1.0: even the 0.9 one no longer clears it
}

TEST(EvaluationTest, BuildUniversesPartitionsByStatusAndPattern) {
    SimulationOutput simulation;
    simulation.orders = {
        make_new_order("ORD-1", AbusePattern::kFrontRunning),
        make_new_order("ORD-2", AbusePattern::kBaseline),
        make_cancel_order("ORD-3", AbusePattern::kSpoofingLayering),
    };
    simulation.executions = {
        make_execution("EXE-1", AbusePattern::kWashTrade),
        make_execution("EXE-2", AbusePattern::kBaseline),
    };

    tse::harness::Universes universes = tse::harness::build_universes(simulation);

    EXPECT_EQ(universes.new_orders.event_ids.size(), 2u);
    EXPECT_EQ(universes.cancel_orders.event_ids.size(), 1u);
    EXPECT_EQ(universes.executions.event_ids.size(), 2u);

    const auto& front_running_ids = tse::harness::positive_ids_for(universes.new_orders, AbusePattern::kFrontRunning);
    EXPECT_EQ(front_running_ids, (std::unordered_set<std::string>{"ORD-1"}));

    const auto& spoof_ids = tse::harness::positive_ids_for(universes.cancel_orders, AbusePattern::kSpoofingLayering);
    EXPECT_EQ(spoof_ids, (std::unordered_set<std::string>{"ORD-3"}));

    // A pattern that never occurs in this universe returns an empty set, not
    // a missing-key crash.
    const auto& mtc_ids_in_new_orders = tse::harness::positive_ids_for(universes.new_orders, AbusePattern::kMarkingTheClose);
    EXPECT_TRUE(mtc_ids_in_new_orders.empty());
}

TEST(EvaluationTest, AnyAbuseIdsExcludesBaselineOnly) {
    SimulationOutput simulation;
    simulation.executions = {
        make_execution("EXE-1", AbusePattern::kWashTrade),
        make_execution("EXE-2", AbusePattern::kMarkingTheClose),
        make_execution("EXE-3", AbusePattern::kBaseline),
    };
    tse::harness::Universes universes = tse::harness::build_universes(simulation);

    auto abuse_ids = tse::harness::any_abuse_ids(universes.executions);

    EXPECT_EQ(abuse_ids, (std::unordered_set<std::string>{"EXE-1", "EXE-2"}));
}

TEST(EvaluationTest, DetectorUniverseKindMatchesEachDetectorsOwnReactivity) {
    EXPECT_EQ(tse::harness::detector_universe_kind("WashTradeDetector"), tse::harness::UniverseKind::kExecution);
    EXPECT_EQ(tse::harness::detector_universe_kind("SpoofingLayeringDetector"), tse::harness::UniverseKind::kCancelOrder);
    EXPECT_EQ(tse::harness::detector_universe_kind("MarkingTheCloseDetector"), tse::harness::UniverseKind::kExecution);
    EXPECT_EQ(tse::harness::detector_universe_kind("FrontRunningDetector"), tse::harness::UniverseKind::kNewOrder);
    EXPECT_EQ(tse::harness::detector_universe_kind("StatisticalBaselineDetector"), tse::harness::UniverseKind::kNewOrder);
    EXPECT_THROW(tse::harness::detector_universe_kind("NotARealDetector"), std::invalid_argument);
}
