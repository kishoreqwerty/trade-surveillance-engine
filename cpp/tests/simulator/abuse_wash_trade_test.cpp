#include <gtest/gtest.h>

#include <random>

#include "abuse/wash_trade.hpp"
#include "instrument_universe.hpp"

using namespace tse::simulator;

namespace {
Instrument make_instrument() {
    return build_instrument_universe({1, 0, 0, 0}).front();
}
Account make_account(const std::string& id, const std::string& owner) {
    Account a;
    a.account_id = id;
    a.beneficial_owner_id = owner;
    return a;
}
}  // namespace

TEST(WashTradeInjector, LabelsAllEventsWithSameScenario) {
    std::mt19937_64 rng(1);
    IdGenerator order_gen("ORD"), trade_gen("EXE");
    Instrument instrument = make_instrument();
    Account a = make_account("ACC-A", "OWN-1");
    Account b = make_account("ACC-B", "OWN-1");

    auto scenario = generate_wash_trade_scenario(rng, order_gen, trade_gen, "SCN-WASH-TEST", instrument, a,
                                                  b, 100.0, 0, 0.5, "SIM");

    ASSERT_EQ(scenario.orders.size(), 2u);
    ASSERT_EQ(scenario.executions.size(), 2u);
    for (const auto& order : scenario.orders) {
        EXPECT_EQ(order.ground_truth_label.pattern, AbusePattern::kWashTrade);
        EXPECT_EQ(order.ground_truth_label.scenario_id, "SCN-WASH-TEST");
        EXPECT_DOUBLE_EQ(order.ground_truth_label.severity, 0.5);
    }
    for (const auto& execution : scenario.executions) {
        EXPECT_EQ(execution.ground_truth_label.pattern, AbusePattern::kWashTrade);
        EXPECT_EQ(execution.ground_truth_label.scenario_id, "SCN-WASH-TEST");
    }
}

TEST(WashTradeInjector, ExecutionsCrossBetweenTheTwoLinkedAccounts) {
    std::mt19937_64 rng(2);
    IdGenerator order_gen("ORD"), trade_gen("EXE");
    Instrument instrument = make_instrument();
    Account a = make_account("ACC-A", "OWN-1");
    Account b = make_account("ACC-B", "OWN-1");

    auto scenario = generate_wash_trade_scenario(rng, order_gen, trade_gen, "SCN-WASH-TEST", instrument, a,
                                                  b, 100.0, 0, 0.5, "SIM");

    EXPECT_EQ(scenario.executions[0].account_id, a.account_id);
    EXPECT_EQ(scenario.executions[0].counterparty_account_id, b.account_id);
    EXPECT_EQ(scenario.executions[1].account_id, b.account_id);
    EXPECT_EQ(scenario.executions[1].counterparty_account_id, a.account_id);
}

TEST(WashTradeInjector, MaxSeverityProducesExactMatchAndTightTiming) {
    Instrument instrument = make_instrument();
    Account a = make_account("ACC-A", "OWN-1");
    Account b = make_account("ACC-B", "OWN-1");

    for (uint64_t seed = 0; seed < 10; ++seed) {
        std::mt19937_64 rng(seed);
        IdGenerator order_gen("ORD"), trade_gen("EXE");
        auto scenario = generate_wash_trade_scenario(rng, order_gen, trade_gen, "SCN", instrument, a, b,
                                                       100.0, 0, /*severity=*/1.0, "SIM");
        EXPECT_EQ(scenario.orders[0].qty, scenario.orders[1].qty);
        EXPECT_DOUBLE_EQ(scenario.orders[0].price, scenario.orders[1].price);

        int64_t leg_gap = scenario.orders[1].timestamp_ns - scenario.orders[0].timestamp_ns;
        EXPECT_LT(leg_gap, 1'000'000'000);  // under 1s
    }
}

TEST(WashTradeInjector, LowSeverityProducesLooserTimingThanHighSeverity) {
    Instrument instrument = make_instrument();
    Account a = make_account("ACC-A", "OWN-1");
    Account b = make_account("ACC-B", "OWN-1");

    std::mt19937_64 rng_low(3);
    IdGenerator order_gen_low("ORD"), trade_gen_low("EXE");
    auto low = generate_wash_trade_scenario(rng_low, order_gen_low, trade_gen_low, "SCN-LOW", instrument, a,
                                             b, 100.0, 0, /*severity=*/0.0, "SIM");

    std::mt19937_64 rng_high(3);
    IdGenerator order_gen_high("ORD"), trade_gen_high("EXE");
    auto high = generate_wash_trade_scenario(rng_high, order_gen_high, trade_gen_high, "SCN-HIGH",
                                              instrument, a, b, 100.0, 0, /*severity=*/1.0, "SIM");

    int64_t low_gap = low.orders[1].timestamp_ns - low.orders[0].timestamp_ns;
    int64_t high_gap = high.orders[1].timestamp_ns - high.orders[0].timestamp_ns;
    EXPECT_GT(low_gap, high_gap);
    EXPECT_GT(low_gap, 25'000'000'000);  // >= 25s, matches the severity=0 formula bound
}
