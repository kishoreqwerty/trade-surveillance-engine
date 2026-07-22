#include "marking_the_close_detector.hpp"

#include <algorithm>

#include <gtest/gtest.h>

#include "account_registry.hpp"
#include "order_book.hpp"

using tse::detectors::AccountRegistry;
using tse::detectors::DetectorEvent;
using tse::detectors::Entity;
using tse::detectors::MarkingTheCloseConfig;
using tse::detectors::MarkingTheCloseDetector;
using tse::fix::Execution;
using tse::fix::Side;
using tse::orderbook::OrderBook;

namespace {

constexpr const char* kInstrument = "ACME";
constexpr int64_t kCloseTs = 100'000'000'000LL;  // 100s
constexpr int64_t kWindowStart = 90'000'000'000LL;

Execution make_exec(const std::string& account, const std::string& counterparty, int64_t qty, int64_t ts,
                     const std::string& instrument = kInstrument) {
    Execution execution;
    execution.trade_id = "EXE-" + account + "-" + std::to_string(ts);
    execution.order_id = "O-" + account;
    execution.account_id = account;
    execution.instrument_id = instrument;
    execution.side = Side::kBuy;
    execution.price = 100.00;
    execution.qty = qty;
    execution.timestamp_ns = ts;
    execution.counterparty_account_id = counterparty;
    execution.venue = "SIM";
    return execution;
}

MarkingTheCloseConfig default_config() {
    MarkingTheCloseConfig config;
    config.close_time_ns_by_instrument = {{kInstrument, kCloseTs}};
    config.window_duration_ns = kCloseTs - kWindowStart;  // 10s
    config.concentration_threshold = 0.4;
    config.min_account_qty_threshold = 100;
    config.min_total_window_qty_threshold = 500;
    return config;
}

}  // namespace

TEST(MarkingTheCloseDetector, NameIsMarkingTheCloseDetector) {
    MarkingTheCloseDetector detector(default_config());
    EXPECT_EQ(detector.name(), "MarkingTheCloseDetector");
}

// TP: after enough total window volume accrues to be meaningful, a single
// dominant pair's trade pushes both participants' share past threshold.
TEST(MarkingTheCloseDetector, DominantPairCrossingThresholdFires) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;

    // total window volume: 400 -- below the 500 floor, no check attempted.
    EXPECT_TRUE(detector.evaluate(book, DetectorEvent{make_exec("BASE1", "BASE2", 400, 91'000'000'000)}, accounts)
                    .empty());
    // total: 500 -- floor cleared, but BASE3/BASE4 are only 100/500=0.2 each.
    EXPECT_TRUE(detector.evaluate(book, DetectorEvent{make_exec("BASE3", "BASE4", 100, 92'000'000'000)}, accounts)
                    .empty());

    // total: 1100. DOMINANT and OTHER5 each individually own 600/1100 = 0.5454...
    auto alerts = detector.evaluate(book, DetectorEvent{make_exec("DOMINANT", "OTHER5", 600, 95'000'000'000)},
                                     accounts);
    ASSERT_EQ(alerts.size(), 2u);
    EXPECT_EQ(alerts[0].detector_name, "MarkingTheCloseDetector");
    EXPECT_EQ(alerts[0].account_ids, std::vector<std::string>{"DOMINANT"});
    EXPECT_NEAR(alerts[0].score, 600.0 / 1100.0, 1e-9);
    EXPECT_EQ(alerts[1].account_ids, std::vector<std::string>{"OTHER5"});
    EXPECT_NEAR(alerts[1].score, 600.0 / 1100.0, 1e-9);
}

// Regression: Alert::order_ids must name every trade_id that contributed to
// the firing account's window volume, not stay empty -- the architecture
// doc's evidence contract (alert.hpp: "order/trade IDs that constitute the
// evidence") applies to this detector exactly as much as the other four,
// and this field was previously never populated here at all (found by
// cpp/harness/'s Phase 10 evaluation, which scores Alerts purely off
// Alert::order_ids and got a structural zero true-positive rate for this
// detector until this was fixed).
TEST(MarkingTheCloseDetector, AlertOrderIdsContainsEveryContributingTradeId) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;

    // DOMINANT trades twice before crossing threshold on the second trade --
    // both trade_ids must show up in the eventual alert's order_ids, not
    // just the one that happened to tip it over.
    EXPECT_TRUE(
        detector.evaluate(book, DetectorEvent{make_exec("DOMINANT", "OTHER1", 300, 91'000'000'000)}, accounts)
            .empty());
    auto alerts = detector.evaluate(book, DetectorEvent{make_exec("DOMINANT", "OTHER2", 300, 95'000'000'000)}, accounts);

    ASSERT_EQ(alerts.size(), 2u);  // DOMINANT and OTHER2 both individually clear 300/600 = 0.5
    const auto& dominant_alert = alerts[0].account_ids == std::vector<std::string>{"DOMINANT"} ? alerts[0] : alerts[1];
    EXPECT_EQ(dominant_alert.order_ids,
              (std::vector<std::string>{"EXE-DOMINANT-91000000000", "EXE-DOMINANT-95000000000"}));
}

// TN: broad participation -- nobody individually dominates the close even
// once the total-volume floor is cleared.
TEST(MarkingTheCloseDetector, BroadParticipationBelowConcentrationThresholdDoesNotFire) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;

    EXPECT_TRUE(
        detector.evaluate(book, DetectorEvent{make_exec("P1", "P2", 200, 91'000'000'000)}, accounts).empty());
    EXPECT_TRUE(
        detector.evaluate(book, DetectorEvent{make_exec("P3", "P4", 200, 92'000'000'000)}, accounts).empty());
    // total now 600 (floor cleared); P5/P6 each own 200/600 = 0.333 < 0.4.
    auto alerts = detector.evaluate(book, DetectorEvent{make_exec("P5", "P6", 200, 93'000'000'000)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(MarkingTheCloseDetector, ExecutionBeforeWindowIsIgnored) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;

    auto alerts =
        detector.evaluate(book, DetectorEvent{make_exec("DOMINANT", "OTHER", 100'000, 50'000'000'000)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(MarkingTheCloseDetector, ExecutionAfterCloseIsIgnored) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;

    auto alerts =
        detector.evaluate(book, DetectorEvent{make_exec("DOMINANT", "OTHER", 100'000, kCloseTs + 1)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(MarkingTheCloseDetector, UnknownInstrumentNeverFires) {
    OrderBook book("UNKNOWN");
    MarkingTheCloseDetector detector(default_config());  // only knows ACME's close time
    AccountRegistry accounts;

    auto alerts = detector.evaluate(
        book, DetectorEvent{make_exec("DOMINANT", "OTHER", 100'000, 95'000'000'000, "UNKNOWN")}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(MarkingTheCloseDetector, AlreadyAlertedAccountDoesNotReAlertOnSubsequentQualifyingExecution) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;

    detector.evaluate(book, DetectorEvent{make_exec("BASE1", "BASE2", 400, 91'000'000'000)}, accounts);
    detector.evaluate(book, DetectorEvent{make_exec("BASE3", "BASE4", 100, 92'000'000'000)}, accounts);
    auto first = detector.evaluate(book, DetectorEvent{make_exec("DOMINANT", "OTHER5", 600, 95'000'000'000)},
                                    accounts);
    ASSERT_EQ(first.size(), 2u);  // both DOMINANT and OTHER5 fire the first time

    // DOMINANT and OTHER5 trade again -- both would still individually
    // qualify by share, but both already fired once.
    auto second = detector.evaluate(book, DetectorEvent{make_exec("DOMINANT", "OTHER5", 200, 96'000'000'000)},
                                     accounts);
    EXPECT_TRUE(second.empty());
}

TEST(MarkingTheCloseDetector, IgnoresOrderEventsEntirely) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;

    tse::fix::Order order;
    order.order_id = "O1";
    order.account_id = "DOMINANT";
    order.instrument_id = kInstrument;
    order.side = Side::kBuy;
    order.price = 100.00;
    order.qty = 100'000;
    order.timestamp_ns = 95'000'000'000;
    order.status = tse::fix::OrderStatus::kNew;

    auto alerts = detector.evaluate(book, DetectorEvent{order}, accounts);
    EXPECT_TRUE(alerts.empty());
}

// Phase 11.5: closes the per-account evasion path -- two related accounts
// (same beneficial owner), EACH individually below concentration_threshold,
// EACH trading with its own unrelated counterparty (never with each
// other -- proving grouping doesn't just catch direct wash-trade-shaped
// pairs), must be aggregated into one entity before the concentration
// check.
TEST(MarkingTheCloseDetector, RelatedAccountsSplittingVolumeAreAggregatedAndFireTogether) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;
    accounts.add(Entity{"LINKED-A", "OWNER1", "client", {}});
    accounts.add(Entity{"LINKED-B", "OWNER1", "client", {}});

    // Ambient baseline first (same pattern as DominantPairCrossingThresholdFires
    // above) -- without this, LINKED-A's own first trade would trivially be
    // 100% of window volume so far, the exact degenerate case
    // min_total_window_qty_threshold exists to guard against.
    detector.evaluate(book, DetectorEvent{make_exec("BASE1", "BASE2", 1000, 90'500'000'000)}, accounts);

    // LINKED-A trades with an unrelated counterparty: 500/1500 = 0.333 alone -- below threshold.
    EXPECT_TRUE(
        detector.evaluate(book, DetectorEvent{make_exec("LINKED-A", "OTHER-A", 500, 91'000'000'000)}, accounts)
            .empty());

    // LINKED-B (never trades with LINKED-A) pushes the GROUP's combined
    // share to (500+500)/2000 = 0.5 -- above threshold, must fire.
    auto alerts =
        detector.evaluate(book, DetectorEvent{make_exec("LINKED-B", "OTHER-B", 500, 93'000'000'000)}, accounts);

    ASSERT_EQ(alerts.size(), 1u);  // only LINKED-B's own side fires (OTHER-B alone is well under threshold)
    std::vector<std::string> account_ids = alerts[0].account_ids;
    std::sort(account_ids.begin(), account_ids.end());
    EXPECT_EQ(account_ids, (std::vector<std::string>{"LINKED-A", "LINKED-B"}));
    EXPECT_NEAR(alerts[0].score, 1000.0 / 2000.0, 1e-9);
    std::vector<std::string> order_ids = alerts[0].order_ids;
    std::sort(order_ids.begin(), order_ids.end());
    EXPECT_EQ(order_ids,
              (std::vector<std::string>{"EXE-LINKED-A-91000000000", "EXE-LINKED-B-93000000000"}));
}

// Phase 11.5, the specific case requested for review: a group's alerted_
// status must survive a LATER composition change. LINKED-A and LINKED-B
// fire together as a group; a third account (LINKED-C, related to both,
// but not yet SEEN by the detector) then trades for the first time,
// causing the detector to discover it belongs to the same group. LINKED-C's
// own trade is sized so that, absent the carried-over alerted_ status, the
// newly-merged group's share would clearly clear the threshold again
// (proving this isn't a fire-free coincidence) -- but it must NOT
// spuriously re-fire, because the group it just joined already fired.
TEST(MarkingTheCloseDetector, GroupAlertedStatusSurvivesLaterCompositionChange) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;
    accounts.add(Entity{"LINKED-A", "OWNER1", "client", {}});
    accounts.add(Entity{"LINKED-B", "OWNER1", "client", {}});
    accounts.add(Entity{"LINKED-C", "OWNER1", "client", {}});  // related to A/B from the start in the registry

    // Ambient baseline first -- see the comment in the previous test for why.
    detector.evaluate(book, DetectorEvent{make_exec("BASE1", "BASE2", 1000, 90'500'000'000)}, accounts);

    // LINKED-A alone: 500/1500 = 0.333 -- below threshold, no fire.
    EXPECT_TRUE(
        detector.evaluate(book, DetectorEvent{make_exec("LINKED-A", "OTHER-A", 500, 91'000'000'000)}, accounts)
            .empty());

    // LINKED-B (unseen until now) is discovered related to LINKED-A;
    // combined group share (500+500)/2000 = 0.5 fires.
    auto first =
        detector.evaluate(book, DetectorEvent{make_exec("LINKED-B", "OTHER-B", 500, 93'000'000'000)}, accounts);
    ASSERT_EQ(first.size(), 1u);

    // LINKED-C trades for the first time. The detector discovers (via the
    // registry, not anything that changed) that LINKED-C is also related to
    // LINKED-A/LINKED-B, merging it into the same group. If the newly
    // merged group's alerted_ status did NOT carry over, this would fire
    // again: (500+500+600)/2600 = 0.615, comfortably above 0.4. It must not.
    auto second =
        detector.evaluate(book, DetectorEvent{make_exec("LINKED-C", "OTHER-C", 600, 95'000'000'000)}, accounts);
    EXPECT_TRUE(second.empty())
        << "group must not re-fire when a new member is discovered post-hoc -- the group already fired";
}
