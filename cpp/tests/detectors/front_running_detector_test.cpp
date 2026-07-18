#include "front_running_detector.hpp"

#include <gtest/gtest.h>

#include "account_registry.hpp"
#include "order_book.hpp"

using tse::detectors::AccountRegistry;
using tse::detectors::DetectorEvent;
using tse::detectors::Entity;
using tse::detectors::FrontRunningConfig;
using tse::detectors::FrontRunningDetector;
using tse::fix::Order;
using tse::fix::OrderStatus;
using tse::fix::OrderType;
using tse::fix::Side;
using tse::orderbook::OrderBook;

namespace {

constexpr const char* kInstrument = "ACME";

Order make_new(const std::string& id, const std::string& account, Side side, int64_t qty, int64_t ts) {
    Order order;
    order.order_id = id;
    order.orig_order_id = id;
    order.account_id = account;
    order.instrument_id = kInstrument;
    order.side = side;
    order.price = 100.00;
    order.qty = qty;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = ts;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    return order;
}

AccountRegistry related_registry() {
    AccountRegistry accounts;
    accounts.add(Entity{"LEADER", "OWNER-A", "client", {}});
    accounts.add(Entity{"CLIENT", "OWNER-A", "client", {}});
    return accounts;
}

}  // namespace

TEST(FrontRunningDetector, NameIsFrontRunningDetector) {
    FrontRunningDetector detector;
    EXPECT_EQ(detector.name(), "FrontRunningDetector");
}

TEST(FrontRunningDetector, RelatedSmallerOrderShortlyBeforeLargeOrderFires) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts = related_registry();

    detector.evaluate(book, DetectorEvent{make_new("L1", "LEADER", Side::kBuy, 150, 1'000'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 1'500'000'000)}, accounts);

    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_EQ(alerts[0].detector_name, "FrontRunningDetector");
    EXPECT_DOUBLE_EQ(alerts[0].score, 1.0);
    EXPECT_EQ(alerts[0].instrument_id, kInstrument);
    ASSERT_EQ(alerts[0].account_ids.size(), 2u);
    EXPECT_EQ(alerts[0].account_ids[0], "LEADER");
    EXPECT_EQ(alerts[0].account_ids[1], "CLIENT");
    ASSERT_EQ(alerts[0].order_ids.size(), 2u);
    EXPECT_EQ(alerts[0].order_ids[0], "L1");
    EXPECT_EQ(alerts[0].order_ids[1], "C1");
    EXPECT_EQ(alerts[0].window_start_ns, 1'000'000'000);
    EXPECT_EQ(alerts[0].window_end_ns, 1'500'000'000);
}

TEST(FrontRunningDetector, LeaderTooLargeRelativeToClientOrderDoesNotFire) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts = related_registry();

    // 300 > 1000 * 0.2 (default ratio) -- too large to be "a small leading position"
    detector.evaluate(book, DetectorEvent{make_new("L1", "LEADER", Side::kBuy, 300, 1'000'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 1'500'000'000)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(FrontRunningDetector, LeaderOutsideLookbackWindowDoesNotFire) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;  // default lookback: 2s
    AccountRegistry accounts = related_registry();

    detector.evaluate(book, DetectorEvent{make_new("L1", "LEADER", Side::kBuy, 150, 0)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 3'000'000'000)}, accounts);  // 3s later
    EXPECT_TRUE(alerts.empty());
}

// Regression test for a real bug found via the Phase 9 live dashboard: a
// "leader" entry timestamped *after* the event currently being processed
// (only reachable if the underlying event stream goes non-monotonic --
// e.g. a dropped New under cpp/ingestion/'s drop-oldest backpressure
// policy, or cpp/api/main.cpp's demo feed restarting its synthetic
// clock) must never be treated as a valid leader. Before the fix, the
// prune predicate only ever checked "too old" (a positive, large diff),
// so an entry from the *future* relative to the current order was never
// pruned and lingered indefinitely -- eventually pairing against some
// later order and firing an alert whose evidence text contained a
// nonsensical negative "Xns ahead of" duration.
TEST(FrontRunningDetector, LeaderTimestampedAfterTheLargeOrderNeverFires) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts = related_registry();

    // "Leader" arrives with a LATER timestamp than the large order
    // processed right after it -- impossible for a genuine leader.
    detector.evaluate(book, DetectorEvent{make_new("L1", "LEADER", Side::kBuy, 150, 5'000'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 1'000'000'000)}, accounts);

    EXPECT_TRUE(alerts.empty());
}

// The non-monotonic entry must actually be pruned, not merely skipped for
// one comparison -- a later, genuinely-ordered leader/large pair must
// still fire normally once it is gone, proving the fix doesn't just
// suppress the symptom for a single event.
TEST(FrontRunningDetector, SelfHealsAfterNonMonotonicEntryIsPruned) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts = related_registry();

    detector.evaluate(book, DetectorEvent{make_new("L1", "LEADER", Side::kBuy, 150, 5'000'000'000)}, accounts);
    // Fails to pair with L1 (non-monotonic) *and* prunes L1 as a side
    // effect of processing this order.
    detector.evaluate(book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 1'000'000'000)}, accounts);

    detector.evaluate(book, DetectorEvent{make_new("L2", "LEADER", Side::kBuy, 150, 1'100'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("C2", "CLIENT", Side::kBuy, 1000, 1'500'000'000)}, accounts);

    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_EQ(alerts[0].order_ids[0], "L2") << "a genuinely-ordered leader must still fire once the "
                                                "non-monotonic entry is actually gone, not just skipped";
}

// The leader and the large order it precedes come from two *different*
// accounts -- plausibly different systems with independently-clocked
// infrastructure, unlike SpoofingLayeringDetector's own New/Cancel pair
// (same order_id, same sender, same clock). A leader that's only
// slightly "future"-dated relative to the current order -- small enough
// to plausibly be ordinary cross-account clock skew, not corrupt or
// unrelated data -- must not be permanently discarded just because it
// doesn't pair with *this* particular large order: it must still be
// available to correctly pair with a later large order once real
// elapsed time removes the ambiguity. Permanently pruning it on the
// first skewed comparison (rather than just skipping that one
// comparison) would risk silently suppressing a genuine front-running
// detection.
TEST(FrontRunningDetector, SlightlyFutureLeaderIsNotPermanentlyDiscarded) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts = related_registry();

    // L1 is "future" by 100ms relative to C1 below -- well within the 2s
    // default lookback window, small enough to be ordinary clock skew,
    // unlike LeaderTimestampedAfterTheLargeOrderNeverFires's 4s gap.
    detector.evaluate(book, DetectorEvent{make_new("L1", "LEADER", Side::kBuy, 150, 1'100'000'000)}, accounts);
    auto first_alerts = detector.evaluate(
        book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 1'000'000'000)}, accounts);
    EXPECT_TRUE(first_alerts.empty()) << "must not fire against an apparently-inverted pairing";

    // A later large order, genuinely after L1 by both processing order and
    // timestamp, must still be able to pair with L1 -- proving L1 survived
    // rather than being discarded by the first (skewed) comparison.
    auto second_alerts = detector.evaluate(
        book, DetectorEvent{make_new("C2", "CLIENT", Side::kBuy, 1000, 1'500'000'000)}, accounts);
    ASSERT_EQ(second_alerts.size(), 1u);
    EXPECT_EQ(second_alerts[0].order_ids[0], "L1") << "L1 must not have been permanently discarded by the "
                                                        "first, apparently-inverted comparison against C1";
}

TEST(FrontRunningDetector, UnrelatedAccountsDoNotFire) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts;
    accounts.add(Entity{"LEADER", "OWNER-A", "client", {}});
    accounts.add(Entity{"CLIENT", "OWNER-B", "client", {}});  // different owner, no link

    detector.evaluate(book, DetectorEvent{make_new("L1", "LEADER", Side::kBuy, 150, 1'000'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 1'500'000'000)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(FrontRunningDetector, OppositeSideLeaderDoesNotFire) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts = related_registry();

    detector.evaluate(book, DetectorEvent{make_new("L1", "LEADER", Side::kSell, 150, 1'000'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 1'500'000'000)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(FrontRunningDetector, IncomingOrderBelowLargeThresholdNeverChecked) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;  // default min_large_qty_threshold: 1000
    AccountRegistry accounts = related_registry();

    detector.evaluate(book, DetectorEvent{make_new("L1", "LEADER", Side::kBuy, 150, 1'000'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 999, 1'500'000'000)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(FrontRunningDetector, SameAccountSequencingAgainstItselfDoesNotFire) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts;
    accounts.add(Entity{"SAME_ACC", "OWNER-A", "client", {}});

    detector.evaluate(book, DetectorEvent{make_new("S1", "SAME_ACC", Side::kBuy, 150, 1'000'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("S2", "SAME_ACC", Side::kBuy, 1000, 1'500'000'000)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(FrontRunningDetector, MultipleQualifyingLeadersEachProduceAnAlert) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts;
    accounts.add(Entity{"LEADER1", "OWNER-A", "client", {}});
    accounts.add(Entity{"LEADER2", "OWNER-A", "client", {}});
    accounts.add(Entity{"CLIENT", "OWNER-A", "client", {}});

    detector.evaluate(book, DetectorEvent{make_new("L1", "LEADER1", Side::kBuy, 100, 1'000'000'000)}, accounts);
    detector.evaluate(book, DetectorEvent{make_new("L2", "LEADER2", Side::kBuy, 150, 1'200'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 1'500'000'000)}, accounts);

    ASSERT_EQ(alerts.size(), 2u);
    EXPECT_EQ(alerts[0].account_ids[0], "LEADER1");
    EXPECT_EQ(alerts[1].account_ids[0], "LEADER2");
}

TEST(FrontRunningDetector, IgnoresCancelAndExecutionEvents) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts = related_registry();

    detector.evaluate(book, DetectorEvent{make_new("L1", "LEADER", Side::kBuy, 150, 1'000'000'000)}, accounts);

    Order cancel;
    cancel.order_id = "C0";
    cancel.orig_order_id = "L1";
    cancel.instrument_id = kInstrument;
    cancel.timestamp_ns = 1'200'000'000;
    cancel.status = OrderStatus::kCancelled;
    auto cancel_alerts = detector.evaluate(book, DetectorEvent{cancel}, accounts);
    EXPECT_TRUE(cancel_alerts.empty());

    tse::fix::Execution execution;
    execution.trade_id = "E1";
    execution.order_id = "L1";
    execution.account_id = "LEADER";
    execution.instrument_id = kInstrument;
    execution.side = Side::kBuy;
    execution.price = 100.00;
    execution.qty = 150;
    execution.timestamp_ns = 1'300'000'000;
    execution.venue = "SIM";
    auto exec_alerts = detector.evaluate(book, DetectorEvent{execution}, accounts);
    EXPECT_TRUE(exec_alerts.empty());
}
