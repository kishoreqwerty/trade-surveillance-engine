#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>

#include "abuse/spoofing_layering.hpp"
#include "instrument_universe.hpp"

using namespace tse::simulator;

namespace {
Instrument make_instrument() {
    return build_instrument_universe({1, 0, 0, 0}).front();
}
Account make_account() {
    Account a;
    a.account_id = "ACC-SPOOF";
    a.beneficial_owner_id = "OWN-SPOOF";
    return a;
}
}  // namespace

TEST(SpoofingLayeringInjector, AllEventsShareScenarioLabel) {
    std::mt19937_64 rng(1);
    IdGenerator order_gen("ORD"), trade_gen("EXE");
    auto instrument = make_instrument();
    auto account = make_account();

    auto scenario = generate_spoofing_layering_scenario(rng, order_gen, trade_gen, "SCN-SPOOF-TEST",
                                                          instrument, account, 100.0, 0, 0.5, "SIM");

    // Orders from the dedicated synthetic counterparty ("MKT-COUNTERPARTY",
    // matching spoofing_layering.cpp's own kSyntheticCounterparty) are
    // deliberately excluded here: they're market-structure scaffolding
    // (the move_score anchor mechanism -- see that file's own comment on
    // it), not the spoofing pattern itself, and must NOT carry the
    // scenario's ground truth label -- doing so would make them count as
    // positive-class events the detector can never actually fire on
    // (SpoofingLayeringDetector's alert.order_ids only ever names the
    // layer being cancelled), silently deflating Phase 10's measured
    // recall. Asserted directly below, not just skipped, so a regression
    // that accidentally re-labels them is caught.
    for (const auto& order : scenario.orders) {
        if (order.account_id == "MKT-COUNTERPARTY") {
            EXPECT_EQ(order.ground_truth_label.pattern, AbusePattern::kBaseline)
                << "anchor/market-structure orders must stay unlabeled, not tagged as the scenario itself";
            continue;
        }
        EXPECT_EQ(order.ground_truth_label.pattern, AbusePattern::kSpoofingLayering);
        EXPECT_EQ(order.ground_truth_label.scenario_id, "SCN-SPOOF-TEST");
    }
    for (const auto& execution : scenario.executions) {
        EXPECT_EQ(execution.ground_truth_label.pattern, AbusePattern::kSpoofingLayering);
    }
}

TEST(SpoofingLayeringInjector, EveryLayerIsEventuallyCancelledButGenuineOrderIsNot) {
    std::mt19937_64 rng(2);
    IdGenerator order_gen("ORD"), trade_gen("EXE");
    auto instrument = make_instrument();
    auto account = make_account();

    auto scenario = generate_spoofing_layering_scenario(rng, order_gen, trade_gen, "SCN", instrument,
                                                          account, 100.0, 0, 0.7, "SIM");

    // Restricted to the account under investigation's own orders (the
    // layers + the genuine order) -- excludes the separate, dedicated
    // synthetic-counterparty "anchor" orders (spoofing_layering.cpp's
    // move_score mechanism), which have their own independent
    // New/Cancel/New lifecycle unrelated to this test's actual claim
    // about the layers and the genuine order specifically.
    std::set<std::string> new_order_ids, cancelled_order_ids;
    for (const auto& order : scenario.orders) {
        if (order.account_id != account.account_id) continue;
        if (order.status == OrderStatus::kNew) new_order_ids.insert(order.order_id);
        if (order.status == OrderStatus::kCancelled) cancelled_order_ids.insert(order.order_id);
    }
    // The genuine order (the real, profiting trade) is among the "New" set
    // but is never cancelled — only the spoof layers are.
    EXPECT_EQ(new_order_ids.size(), cancelled_order_ids.size() + 1);
    for (const auto& id : cancelled_order_ids) {
        EXPECT_TRUE(new_order_ids.count(id));
    }
}

TEST(SpoofingLayeringInjector, HigherSeverityMeansMoreLayersLargerSizeAndFasterCancel) {
    auto instrument = make_instrument();
    auto account = make_account();

    std::mt19937_64 rng_low(3);
    IdGenerator order_gen_low("ORD"), trade_gen_low("EXE");
    auto low = generate_spoofing_layering_scenario(rng_low, order_gen_low, trade_gen_low, "SCN-LOW",
                                                    instrument, account, 100.0, 0, 0.0, "SIM");

    std::mt19937_64 rng_high(3);
    IdGenerator order_gen_high("ORD"), trade_gen_high("EXE");
    auto high = generate_spoofing_layering_scenario(rng_high, order_gen_high, trade_gen_high, "SCN-HIGH",
                                                      instrument, account, 100.0, 0, 1.0, "SIM");

    int low_new_count = 0, high_new_count = 0;
    int64_t low_max_qty = 0, high_max_qty = 0;
    for (const auto& o : low.orders) {
        if (o.status == OrderStatus::kNew) {
            ++low_new_count;
            low_max_qty = std::max(low_max_qty, o.qty);
        }
    }
    for (const auto& o : high.orders) {
        if (o.status == OrderStatus::kNew) {
            ++high_new_count;
            high_max_qty = std::max(high_max_qty, o.qty);
        }
    }

    EXPECT_LT(low_new_count, high_new_count);
    EXPECT_LT(low_max_qty, high_max_qty);

    int64_t low_first_cancel = -1, high_first_cancel = -1;
    for (const auto& o : low.orders) {
        if (o.status == OrderStatus::kCancelled) {
            low_first_cancel = o.timestamp_ns;
            break;
        }
    }
    for (const auto& o : high.orders) {
        if (o.status == OrderStatus::kCancelled) {
            high_first_cancel = o.timestamp_ns;
            break;
        }
    }

    ASSERT_GE(low_first_cancel, 0);
    ASSERT_GE(high_first_cancel, 0);
    EXPECT_GT(low_first_cancel, high_first_cancel);
}
