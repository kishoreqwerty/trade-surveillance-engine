#include <gtest/gtest.h>

#include <random>
#include <set>

#include "account_registry.hpp"
#include "baseline_generator.hpp"
#include "id_generator.hpp"
#include "instrument_universe.hpp"

using namespace tse::simulator;

namespace {
std::vector<Instrument> make_test_instruments() {
    return build_instrument_universe({2, 1, 1, 60'000'000'000});
}
}  // namespace

TEST(BaselineGenerator, ProducesEventsWithinSessionWindow) {
    std::mt19937_64 rng(10);
    auto instruments = make_test_instruments();
    AccountRegistry accounts({20, 0}, rng);
    IdGenerator order_gen("ORD"), trade_gen("EXE");

    auto output = generate_baseline_flow({5.0, 0, 60'000'000'000}, instruments, accounts.all(), rng,
                                          order_gen, trade_gen);

    EXPECT_GT(output.orders.size(), 0u);
    for (const auto& order : output.orders) {
        EXPECT_GE(order.timestamp_ns, 0);
        // Cancels may dwell up to ~30s past the arrival that triggered them.
        EXPECT_LT(order.timestamp_ns, 60'000'000'000 + 30'000'000'000);
    }
    for (const auto& execution : output.executions) {
        EXPECT_GE(execution.timestamp_ns, 0);
    }
}

TEST(BaselineGenerator, EveryEventCarriesBaselineLabel) {
    std::mt19937_64 rng(11);
    auto instruments = make_test_instruments();
    AccountRegistry accounts({20, 0}, rng);
    IdGenerator order_gen("ORD"), trade_gen("EXE");

    auto output = generate_baseline_flow({5.0, 0, 30'000'000'000}, instruments, accounts.all(), rng,
                                          order_gen, trade_gen);

    ASSERT_GT(output.orders.size(), 0u);
    for (const auto& order : output.orders) {
        EXPECT_EQ(order.ground_truth_label.pattern, AbusePattern::kBaseline);
    }
    for (const auto& execution : output.executions) {
        EXPECT_EQ(execution.ground_truth_label.pattern, AbusePattern::kBaseline);
    }
}

TEST(BaselineGenerator, CoversMultipleAssetClasses) {
    std::mt19937_64 rng(12);
    auto instruments = make_test_instruments();
    AccountRegistry accounts({20, 0}, rng);
    IdGenerator order_gen("ORD"), trade_gen("EXE");

    auto output = generate_baseline_flow({20.0, 0, 60'000'000'000}, instruments, accounts.all(), rng,
                                          order_gen, trade_gen);

    std::set<std::string> instruments_seen;
    for (const auto& order : output.orders) instruments_seen.insert(order.instrument_id);
    EXPECT_GE(instruments_seen.size(), 2u);
}

TEST(BaselineGenerator, HigherRateProducesMoreEvents) {
    auto instruments = make_test_instruments();

    std::mt19937_64 rng_low(13);
    AccountRegistry accounts_low({20, 0}, rng_low);
    IdGenerator order_gen_low("ORD"), trade_gen_low("EXE");
    auto low = generate_baseline_flow({1.0, 0, 60'000'000'000}, instruments, accounts_low.all(), rng_low,
                                       order_gen_low, trade_gen_low);

    std::mt19937_64 rng_high(14);
    AccountRegistry accounts_high({20, 0}, rng_high);
    IdGenerator order_gen_high("ORD"), trade_gen_high("EXE");
    auto high = generate_baseline_flow({20.0, 0, 60'000'000'000}, instruments, accounts_high.all(), rng_high,
                                        order_gen_high, trade_gen_high);

    EXPECT_GT(high.orders.size(), low.orders.size());
}
