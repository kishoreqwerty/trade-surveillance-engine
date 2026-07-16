#include <gtest/gtest.h>

#include <algorithm>
#include <set>

#include "serialization/fix_writer.hpp"
#include "simulator.hpp"

using namespace tse::simulator;

namespace {
SimulatorConfig make_small_config() {
    SimulatorConfig config;
    config.random_seed = 99;
    config.session_duration_ns = 120'000'000'000;  // 2 minutes
    config.baseline_orders_per_second = 3.0;
    config.num_equity_instruments = 2;
    config.num_fx_instruments = 1;
    config.num_fixed_income_instruments = 1;
    config.num_independent_accounts = 20;
    config.num_linked_account_pairs = 6;
    return config;
}
}  // namespace

TEST(Simulator, InjectsExactlyTheConfiguredNumberOfScenariosPerPattern) {
    SimulatorConfig config = make_small_config();
    config.wash_trade = {3, 0.5};
    config.spoofing_layering = {2, 0.5};
    config.marking_the_close = {4, 0.5};
    config.front_running = {5, 0.5};

    auto output = generate_simulation(config);

    auto count_distinct_scenarios = [&](AbusePattern pattern) {
        std::set<std::string> ids;
        for (const auto& o : output.orders) {
            if (o.ground_truth_label.pattern == pattern) ids.insert(o.ground_truth_label.scenario_id);
        }
        return ids.size();
    };

    EXPECT_EQ(count_distinct_scenarios(AbusePattern::kWashTrade), 3u);
    EXPECT_EQ(count_distinct_scenarios(AbusePattern::kSpoofingLayering), 2u);
    EXPECT_EQ(count_distinct_scenarios(AbusePattern::kMarkingTheClose), 4u);
    EXPECT_EQ(count_distinct_scenarios(AbusePattern::kFrontRunning), 5u);
}

TEST(Simulator, OrdersAndExecutionsAreTimeSorted) {
    SimulatorConfig config = make_small_config();
    config.wash_trade = {2, 0.5};
    config.spoofing_layering = {2, 0.5};

    auto output = generate_simulation(config);

    EXPECT_TRUE(std::is_sorted(
        output.orders.begin(), output.orders.end(),
        [](const Order& a, const Order& b) { return a.timestamp_ns < b.timestamp_ns; }));
    EXPECT_TRUE(std::is_sorted(
        output.executions.begin(), output.executions.end(),
        [](const Execution& a, const Execution& b) { return a.timestamp_ns < b.timestamp_ns; }));
}

TEST(Simulator, CoversAllThreeAssetClasses) {
    auto output = generate_simulation(make_small_config());
    std::set<AssetClass> classes;
    for (const auto& instrument : output.instruments) classes.insert(instrument.asset_class);
    EXPECT_EQ(classes.size(), 3u);
}

TEST(Simulator, BaselineAndAbuseEventsCoexist) {
    SimulatorConfig config = make_small_config();
    config.wash_trade = {2, 0.5};

    auto output = generate_simulation(config);

    bool has_baseline = false, has_abuse = false;
    for (const auto& o : output.orders) {
        if (o.ground_truth_label.pattern == AbusePattern::kBaseline) has_baseline = true;
        if (is_abuse(o.ground_truth_label)) has_abuse = true;
    }
    EXPECT_TRUE(has_baseline);
    EXPECT_TRUE(has_abuse);
}

TEST(Simulator, DeterministicForFixedSeed) {
    SimulatorConfig config = make_small_config();
    config.wash_trade = {2, 0.5};

    auto output_a = generate_simulation(config);
    auto output_b = generate_simulation(config);

    ASSERT_EQ(output_a.orders.size(), output_b.orders.size());
    for (size_t i = 0; i < output_a.orders.size(); ++i) {
        EXPECT_EQ(output_a.orders[i].order_id, output_b.orders[i].order_id);
        EXPECT_EQ(output_a.orders[i].timestamp_ns, output_b.orders[i].timestamp_ns);
    }
}

// The above only checks the internal Order/Execution structs. Phase 3's
// Kafka replay determinism claim ("same input -> same detector output")
// depends on the actual bytes fed into the pipeline being identical run to
// run, i.e. the rendered FIX text — not just the structs it was built from.
// This regenerates the full session twice (all four abuse patterns, not
// just wash trade) and asserts the rendered FIX message vectors are
// byte-for-byte identical.
TEST(Simulator, FixOutputDeterministicForFixedSeed) {
    SimulatorConfig config = make_small_config();
    config.wash_trade = {2, 0.5};
    config.spoofing_layering = {2, 0.5};
    config.marking_the_close = {2, 0.5};
    config.front_running = {2, 0.5};

    auto output_a = generate_simulation(config);
    auto output_b = generate_simulation(config);

    auto fix_a = to_fix_messages(output_a.orders, output_a.executions);
    auto fix_b = to_fix_messages(output_b.orders, output_b.executions);

    ASSERT_EQ(fix_a.size(), fix_b.size());
    ASSERT_GT(fix_a.size(), 0u);
    for (size_t i = 0; i < fix_a.size(); ++i) {
        EXPECT_EQ(fix_a[i], fix_b[i]) << "FIX message " << i << " diverged between identically-seeded runs";
    }
}
