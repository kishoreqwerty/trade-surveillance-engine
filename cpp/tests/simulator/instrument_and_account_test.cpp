#include <gtest/gtest.h>

#include <random>
#include <set>

#include "account_registry.hpp"
#include "instrument_universe.hpp"

using namespace tse::simulator;

TEST(InstrumentUniverse, ProducesRequestedCountsPerAssetClass) {
    auto instruments = build_instrument_universe({3, 2, 2, 1'000'000});
    int equity = 0, fx = 0, fixed_income = 0;
    for (const auto& i : instruments) {
        switch (i.asset_class) {
            case AssetClass::kEquity: ++equity; break;
            case AssetClass::kFx: ++fx; break;
            case AssetClass::kFixedIncome: ++fixed_income; break;
        }
    }
    EXPECT_EQ(equity, 3);
    EXPECT_EQ(fx, 2);
    EXPECT_EQ(fixed_income, 2);
}

TEST(InstrumentUniverse, InstrumentIdsAreUnique) {
    auto instruments = build_instrument_universe({5, 4, 4, 0});
    std::set<std::string> ids;
    for (const auto& i : instruments) ids.insert(i.instrument_id);
    EXPECT_EQ(ids.size(), instruments.size());
}

TEST(InstrumentUniverse, ReferencePriceIsPositive) {
    auto instruments = build_instrument_universe({1, 1, 1, 0});
    for (const auto& i : instruments) {
        EXPECT_GT(reference_price(i), 0.0);
    }
}

TEST(AccountRegistry, ProducesRequestedIndependentAndLinkedCounts) {
    std::mt19937_64 rng(1);
    AccountRegistry registry({10, 4}, rng);
    // 10 independent + 4 linked pairs * 2 accounts each = 18
    EXPECT_EQ(registry.all().size(), 18u);
}

TEST(AccountRegistry, LinkedPairsShareBeneficialOwnerAndReferenceEachOther) {
    std::mt19937_64 rng(2);
    AccountRegistry registry({5, 3}, rng);
    for (int i = 0; i < 20; ++i) {
        auto pair = registry.random_linked_pair(rng);
        EXPECT_EQ(pair.first.beneficial_owner_id, pair.second.beneficial_owner_id);
        ASSERT_FALSE(pair.first.linked_account_ids.empty());
        EXPECT_EQ(pair.first.linked_account_ids.front(), pair.second.account_id);
        ASSERT_FALSE(pair.second.linked_account_ids.empty());
        EXPECT_EQ(pair.second.linked_account_ids.front(), pair.first.account_id);
    }
}

TEST(AccountRegistry, IndependentAccountsHaveNoLinks) {
    std::mt19937_64 rng(3);
    AccountRegistry registry({5, 0}, rng);
    for (int i = 0; i < 10; ++i) {
        const Account& a = registry.random_independent(rng);
        EXPECT_TRUE(a.linked_account_ids.empty());
    }
}
