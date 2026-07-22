#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>

#include "abuse/marking_the_close.hpp"
#include "instrument_universe.hpp"
#include "simulator.hpp"

using namespace tse::simulator;

namespace {
Instrument make_instrument(int64_t close_ns) {
    return build_instrument_universe({1, 0, 0, close_ns}).front();
}
std::vector<Account> make_accounts(int n) {
    std::vector<Account> accounts;
    for (int i = 0; i < n; ++i) {
        Account a;
        a.account_id = "ACC-" + std::to_string(i);
        a.beneficial_owner_id = "OWN-" + std::to_string(i);
        accounts.push_back(a);
    }
    return accounts;
}
}  // namespace

TEST(MarkingTheCloseInjector, AllTradesLandBeforeSessionCloseAndCarryLabel) {
    std::mt19937_64 rng(1);
    IdGenerator order_gen("ORD"), trade_gen("EXE");
    int64_t close_ns = 300'000'000'000;
    auto instrument = make_instrument(close_ns);
    auto accounts = make_accounts(3);

    auto scenario = generate_marking_the_close_scenario(rng, order_gen, trade_gen, "SCN-MTC-TEST", instrument,
                                                          accounts, 100.0, 0.5,
                                                          kEmpiricalOrderArrivalRatePerInstrumentPerSecond, 1, "SIM");

    ASSERT_FALSE(scenario.orders.empty());
    for (const auto& order : scenario.orders) {
        EXPECT_LT(order.timestamp_ns, close_ns);
        EXPECT_EQ(order.ground_truth_label.pattern, AbusePattern::kMarkingTheClose);
        EXPECT_EQ(order.ground_truth_label.scenario_id, "SCN-MTC-TEST");
    }
}

TEST(MarkingTheCloseInjector, HigherSeverityClustersTighterToCloseWithFewerAccountsAndMoreTrades) {
    int64_t close_ns = 300'000'000'000;
    auto instrument = make_instrument(close_ns);
    auto accounts = make_accounts(5);

    std::mt19937_64 rng_low(2);
    IdGenerator order_gen_low("ORD"), trade_gen_low("EXE");
    auto low = generate_marking_the_close_scenario(rng_low, order_gen_low, trade_gen_low, "SCN-LOW", instrument,
                                                    accounts, 100.0, 0.0,
                                                    kEmpiricalOrderArrivalRatePerInstrumentPerSecond, 1, "SIM");

    std::mt19937_64 rng_high(2);
    IdGenerator order_gen_high("ORD"), trade_gen_high("EXE");
    auto high = generate_marking_the_close_scenario(rng_high, order_gen_high, trade_gen_high, "SCN-HIGH", instrument,
                                                      accounts, 100.0, 1.0,
                                                      kEmpiricalOrderArrivalRatePerInstrumentPerSecond, 1, "SIM");

    int64_t low_earliest = low.orders.front().timestamp_ns;
    int64_t high_earliest = high.orders.front().timestamp_ns;
    for (const auto& o : low.orders) low_earliest = std::min(low_earliest, o.timestamp_ns);
    for (const auto& o : high.orders) high_earliest = std::min(high_earliest, o.timestamp_ns);

    // High severity's earliest trade lands closer to the close than low severity's.
    EXPECT_LT(close_ns - high_earliest, close_ns - low_earliest);

    std::set<std::string> low_accounts, high_accounts;
    for (const auto& o : low.orders) low_accounts.insert(o.account_id);
    for (const auto& o : high.orders) high_accounts.insert(o.account_id);
    EXPECT_LE(high_accounts.size(), low_accounts.size());

    EXPECT_GT(high.orders.size(), low.orders.size());
}
