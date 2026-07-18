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
    accounts.add(Entity{"CLIENT", "OWNER-A", "client", {}});
    accounts.add(Entity{"RELATED", "OWNER-A", "client", {}});
    return accounts;
}

}  // namespace

TEST(FrontRunningDetector, NameIsFrontRunningDetector) {
    FrontRunningDetector detector;
    EXPECT_EQ(detector.name(), "FrontRunningDetector");
}

// The corrected, standard-definition direction: a large order is placed
// first (it becomes "known" to anyone watching order flow), and a related
// account's smaller order reacts shortly AFTER -- trading ahead of the
// large order's own (typically much later) fill and price impact. This is
// also exactly what cpp/simulator/abuse/front_running.cpp generates (see
// its own "how quickly the related account trades after the client order
// is placed" comment).
TEST(FrontRunningDetector, RelatedSmallerOrderShortlyAfterLargeOrderFires) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts = related_registry();

    detector.evaluate(book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 1'000'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("R1", "RELATED", Side::kBuy, 150, 1'500'000'000)}, accounts);

    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_EQ(alerts[0].detector_name, "FrontRunningDetector");
    EXPECT_DOUBLE_EQ(alerts[0].score, 1.0);
    EXPECT_EQ(alerts[0].instrument_id, kInstrument);
    ASSERT_EQ(alerts[0].account_ids.size(), 2u);
    EXPECT_EQ(alerts[0].account_ids[0], "CLIENT");
    EXPECT_EQ(alerts[0].account_ids[1], "RELATED");
    ASSERT_EQ(alerts[0].order_ids.size(), 2u);
    EXPECT_EQ(alerts[0].order_ids[0], "C1");
    EXPECT_EQ(alerts[0].order_ids[1], "R1");
    EXPECT_EQ(alerts[0].window_start_ns, 1'000'000'000);
    EXPECT_EQ(alerts[0].window_end_ns, 1'500'000'000);
}

TEST(FrontRunningDetector, ReactingOrderTooLargeRelativeToClientOrderDoesNotFire) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts = related_registry();

    detector.evaluate(book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 1'000'000'000)}, accounts);
    // 300 > 1000 * 0.2 (default ratio) -- too large to be "a small reacting position"
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("R1", "RELATED", Side::kBuy, 300, 1'500'000'000)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(FrontRunningDetector, ReactionOutsideLookbackWindowDoesNotFire) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;  // default lookback: 2s
    AccountRegistry accounts = related_registry();

    detector.evaluate(book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 0)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("R1", "RELATED", Side::kBuy, 150, 3'000'000'000)}, accounts);  // 3s later
    EXPECT_TRUE(alerts.empty());
}

// Regression: a "large predecessor" entry timestamped *after* the order
// currently being evaluated (only reachable if the underlying event stream
// goes non-monotonic -- e.g. a dropped New under cpp/ingestion/'s
// drop-oldest backpressure policy, or cpp/api/main.cpp's demo feed
// restarting its synthetic clock) must never be treated as valid: it would
// mean the "large" order hasn't been placed yet from the current order's
// perspective, so there's no advance knowledge for the current order to
// have acted on.
TEST(FrontRunningDetector, LargeOrderTimestampedAfterTheReactingOrderNeverFires) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts = related_registry();

    // "Large predecessor" arrives with a LATER timestamp than the order
    // processed right after it -- impossible for a genuine reaction.
    detector.evaluate(book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 5'000'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("R1", "RELATED", Side::kBuy, 150, 1'000'000'000)}, accounts);

    EXPECT_TRUE(alerts.empty());
}

// The non-monotonic entry must actually be pruned (its 4s gap exceeds the
// 2s lookback window), not merely skipped for one comparison -- a later,
// genuinely-ordered large/reacting pair must still fire normally once it
// is gone, proving the fix doesn't just suppress the symptom for a single
// event.
TEST(FrontRunningDetector, SelfHealsAfterNonMonotonicEntryIsPruned) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts = related_registry();

    detector.evaluate(book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 5'000'000'000)}, accounts);
    // Fails to pair with C1 (non-monotonic, and outside the 2s lookback
    // window besides) *and* prunes C1 as a side effect of processing this
    // order.
    detector.evaluate(book, DetectorEvent{make_new("R1", "RELATED", Side::kBuy, 150, 1'000'000'000)}, accounts);

    detector.evaluate(book, DetectorEvent{make_new("C2", "CLIENT", Side::kBuy, 1000, 1'100'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("R2", "RELATED", Side::kBuy, 150, 1'500'000'000)}, accounts);

    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_EQ(alerts[0].order_ids[0], "C2") << "a genuinely-ordered large predecessor must still fire once the "
                                                "non-monotonic entry is actually gone, not just skipped";
}

// The large order and the related account reacting to it come from two
// *different* accounts -- plausibly different systems with independently-
// clocked infrastructure, unlike SpoofingLayeringDetector's own New/Cancel
// pair (same order_id, same sender, same clock). A large order that only
// *appears* slightly "future"-dated relative to a related order comparing
// against it -- small enough to plausibly be ordinary cross-account clock
// skew, not corrupt or unrelated data -- must not be permanently discarded
// just because it doesn't pair with *that* particular reacting order: it
// must still be available to correctly pair with a later reacting order
// once real elapsed time removes the ambiguity.
TEST(FrontRunningDetector, LargeOrderSurvivesASlightlySkewedInvertedComparison) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts = related_registry();

    // C1 is "future" by 100ms relative to R1 below -- well within the 2s
    // default lookback window, small enough to be ordinary clock skew,
    // unlike LargeOrderTimestampedAfterTheReactingOrderNeverFires's 4s gap.
    detector.evaluate(book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 1'100'000'000)}, accounts);
    auto first_alerts = detector.evaluate(
        book, DetectorEvent{make_new("R1", "RELATED", Side::kBuy, 150, 1'000'000'000)}, accounts);
    EXPECT_TRUE(first_alerts.empty()) << "must not fire against an apparently-inverted pairing";

    // A later reacting order, genuinely after C1 by both processing order
    // and timestamp, must still be able to pair with C1 -- proving C1
    // survived rather than being discarded by the first (skewed)
    // comparison.
    auto second_alerts = detector.evaluate(
        book, DetectorEvent{make_new("R2", "RELATED", Side::kBuy, 150, 1'500'000'000)}, accounts);
    ASSERT_EQ(second_alerts.size(), 1u);
    EXPECT_EQ(second_alerts[0].order_ids[0], "C1") << "C1 must not have been permanently discarded by the "
                                                        "first, apparently-inverted comparison against R1";
}

TEST(FrontRunningDetector, UnrelatedAccountsDoNotFire) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts;
    accounts.add(Entity{"CLIENT", "OWNER-A", "client", {}});
    accounts.add(Entity{"RELATED", "OWNER-B", "client", {}});  // different owner, no link

    detector.evaluate(book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 1'000'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("R1", "RELATED", Side::kBuy, 150, 1'500'000'000)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(FrontRunningDetector, OppositeSideDoesNotFire) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts = related_registry();

    detector.evaluate(book, DetectorEvent{make_new("C1", "CLIENT", Side::kSell, 1000, 1'000'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("R1", "RELATED", Side::kBuy, 150, 1'500'000'000)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(FrontRunningDetector, PriorOrderBelowLargeThresholdNeverActsAsALargePredecessor) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;  // default min_large_qty_threshold: 1000
    AccountRegistry accounts = related_registry();

    detector.evaluate(book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 999, 1'000'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("R1", "RELATED", Side::kBuy, 150, 1'500'000'000)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(FrontRunningDetector, SameAccountSequencingAgainstItselfDoesNotFire) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts;
    accounts.add(Entity{"SAME_ACC", "OWNER-A", "client", {}});

    detector.evaluate(book, DetectorEvent{make_new("S1", "SAME_ACC", Side::kBuy, 1000, 1'000'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("S2", "SAME_ACC", Side::kBuy, 150, 1'500'000'000)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(FrontRunningDetector, MultipleQualifyingLargePredecessorsEachProduceAnAlert) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts;
    accounts.add(Entity{"CLIENT1", "OWNER-A", "client", {}});
    accounts.add(Entity{"CLIENT2", "OWNER-A", "client", {}});
    accounts.add(Entity{"RELATED", "OWNER-A", "client", {}});

    detector.evaluate(book, DetectorEvent{make_new("C1", "CLIENT1", Side::kBuy, 1000, 1'000'000'000)}, accounts);
    detector.evaluate(book, DetectorEvent{make_new("C2", "CLIENT2", Side::kBuy, 1500, 1'200'000'000)}, accounts);
    auto alerts = detector.evaluate(
        book, DetectorEvent{make_new("R1", "RELATED", Side::kBuy, 150, 1'500'000'000)}, accounts);

    ASSERT_EQ(alerts.size(), 2u);
    EXPECT_EQ(alerts[0].account_ids[0], "CLIENT1");
    EXPECT_EQ(alerts[1].account_ids[0], "CLIENT2");
}

TEST(FrontRunningDetector, IgnoresCancelAndExecutionEvents) {
    OrderBook book(kInstrument);
    FrontRunningDetector detector;
    AccountRegistry accounts = related_registry();

    detector.evaluate(book, DetectorEvent{make_new("C1", "CLIENT", Side::kBuy, 1000, 1'000'000'000)}, accounts);

    Order cancel;
    cancel.order_id = "X0";
    cancel.orig_order_id = "C1";
    cancel.instrument_id = kInstrument;
    cancel.timestamp_ns = 1'200'000'000;
    cancel.status = OrderStatus::kCancelled;
    auto cancel_alerts = detector.evaluate(book, DetectorEvent{cancel}, accounts);
    EXPECT_TRUE(cancel_alerts.empty());

    tse::fix::Execution execution;
    execution.trade_id = "E1";
    execution.order_id = "C1";
    execution.account_id = "CLIENT";
    execution.instrument_id = kInstrument;
    execution.side = Side::kBuy;
    execution.price = 100.00;
    execution.qty = 1000;
    execution.timestamp_ns = 1'300'000'000;
    execution.venue = "SIM";
    auto exec_alerts = detector.evaluate(book, DetectorEvent{execution}, accounts);
    EXPECT_TRUE(exec_alerts.empty());
}
