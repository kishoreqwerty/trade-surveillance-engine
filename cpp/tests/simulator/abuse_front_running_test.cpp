#include <gtest/gtest.h>

#include <random>

#include "abuse/front_running.hpp"
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

TEST(FrontRunningInjector, RelatedAccountTradesBeforeClientFillAndCarriesLabel) {
    std::mt19937_64 rng(1);
    IdGenerator order_gen("ORD"), trade_gen("EXE");
    auto instrument = make_instrument();
    auto client = make_account("ACC-CLIENT", "OWN-1");
    auto related = make_account("ACC-RELATED", "OWN-1");

    auto scenario = generate_front_running_scenario(rng, order_gen, trade_gen, "SCN-FR-TEST", instrument,
                                                      client, related, 100.0, 0, 0.5, "SIM");

    ASSERT_GE(scenario.executions.size(), 2u);
    const Execution& related_exec = scenario.executions[0];
    const Execution& client_exec = scenario.executions[1];
    EXPECT_EQ(related_exec.account_id, related.account_id);
    EXPECT_EQ(client_exec.account_id, client.account_id);
    EXPECT_LT(related_exec.timestamp_ns, client_exec.timestamp_ns);

    for (const auto& order : scenario.orders) {
        EXPECT_EQ(order.ground_truth_label.pattern, AbusePattern::kFrontRunning);
        EXPECT_EQ(order.ground_truth_label.scenario_id, "SCN-FR-TEST");
    }
}

TEST(FrontRunningInjector, ReversalLegPresentOnlyAtHighSeverity) {
    auto instrument = make_instrument();
    auto client = make_account("ACC-CLIENT", "OWN-1");
    auto related = make_account("ACC-RELATED", "OWN-1");

    std::mt19937_64 rng_low(2);
    IdGenerator order_gen_low("ORD"), trade_gen_low("EXE");
    auto low = generate_front_running_scenario(rng_low, order_gen_low, trade_gen_low, "SCN-LOW", instrument,
                                                 client, related, 100.0, 0, 0.2, "SIM");

    std::mt19937_64 rng_high(2);
    IdGenerator order_gen_high("ORD"), trade_gen_high("EXE");
    auto high = generate_front_running_scenario(rng_high, order_gen_high, trade_gen_high, "SCN-HIGH",
                                                  instrument, client, related, 100.0, 0, 0.8, "SIM");

    EXPECT_EQ(low.orders.size(), 2u);   // client + related only
    EXPECT_EQ(low.executions.size(), 2u);
    EXPECT_EQ(high.orders.size(), 3u);  // + reversal
    EXPECT_EQ(high.executions.size(), 3u);
}

TEST(FrontRunningInjector, HigherSeverityMeansShorterLeadTime) {
    auto instrument = make_instrument();
    auto client = make_account("ACC-CLIENT", "OWN-1");
    auto related = make_account("ACC-RELATED", "OWN-1");

    std::mt19937_64 rng_low(3);
    IdGenerator order_gen_low("ORD"), trade_gen_low("EXE");
    auto low = generate_front_running_scenario(rng_low, order_gen_low, trade_gen_low, "SCN-LOW", instrument,
                                                 client, related, 100.0, 0, 0.0, "SIM");

    std::mt19937_64 rng_high(3);
    IdGenerator order_gen_high("ORD"), trade_gen_high("EXE");
    auto high = generate_front_running_scenario(rng_high, order_gen_high, trade_gen_high, "SCN-HIGH",
                                                  instrument, client, related, 100.0, 0, 1.0, "SIM");

    int64_t low_lead = low.executions[1].timestamp_ns;   // anchor_time_ns == 0
    int64_t high_lead = high.executions[1].timestamp_ns;
    EXPECT_GT(low_lead, high_lead);
}
