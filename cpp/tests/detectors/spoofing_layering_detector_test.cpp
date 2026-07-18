#include "spoofing_layering_detector.hpp"

#include <gtest/gtest.h>

#include "account_registry.hpp"
#include "order_book.hpp"

using tse::detectors::AccountRegistry;
using tse::detectors::Alert;
using tse::detectors::DetectorEvent;
using tse::detectors::SpoofingLayeringConfig;
using tse::detectors::SpoofingLayeringDetector;
using tse::fix::Execution;
using tse::fix::Order;
using tse::fix::OrderStatus;
using tse::fix::OrderType;
using tse::fix::Side;
using tse::orderbook::OrderBook;

namespace {

constexpr const char* kInstrument = "ACME";

Order make_new(const std::string& id, const std::string& account, Side side, double price, int64_t qty,
               int64_t ts) {
    Order order;
    order.order_id = id;
    order.orig_order_id = id;
    order.account_id = account;
    order.instrument_id = kInstrument;
    order.side = side;
    order.price = price;
    order.qty = qty;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = ts;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    return order;
}

Order make_cancel(const std::string& new_id, const std::string& target, int64_t ts) {
    Order order;
    order.order_id = new_id;
    order.orig_order_id = target;
    order.instrument_id = kInstrument;
    order.timestamp_ns = ts;
    order.status = OrderStatus::kCancelled;
    return order;
}

Order make_replace(const std::string& new_id, const std::string& target, const std::string& account, Side side,
                    double price, int64_t qty, int64_t ts) {
    Order order;
    order.order_id = new_id;
    order.orig_order_id = target;
    order.account_id = account;
    order.instrument_id = kInstrument;
    order.side = side;
    order.price = price;
    order.qty = qty;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = ts;
    order.status = OrderStatus::kReplaced;
    return order;
}

Execution make_execution(const std::string& order_id, int64_t qty, int64_t ts) {
    Execution execution;
    execution.trade_id = "EXE-" + order_id;
    execution.order_id = order_id;
    execution.account_id = "IRRELEVANT";  // SpoofingLayeringDetector tracks by order_id, not account, for fills
    execution.instrument_id = kInstrument;
    execution.side = Side::kBuy;
    execution.price = 99.00;
    execution.qty = qty;
    execution.timestamp_ns = ts;
    execution.venue = "SIM";
    return execution;
}

// Applies the event to a REAL OrderBook first (mirroring the live
// pipeline's "orderbook applies the update, then detectors evaluate"
// ordering) and only then hands the resulting book state to the detector —
// every best_price()/qty_at_price() query the detector makes reflects
// genuine, already-verified Phase 4 book state, not a mock.
std::vector<Alert> apply_and_evaluate(OrderBook& book, SpoofingLayeringDetector& detector, const Order& order,
                                      const AccountRegistry& accounts) {
    book.apply(order);
    return detector.evaluate(book, DetectorEvent{order}, accounts);
}

std::vector<Alert> apply_and_evaluate(OrderBook& book, SpoofingLayeringDetector& detector, const Execution& execution,
                                      const AccountRegistry& accounts) {
    book.apply(execution);
    return detector.evaluate(book, DetectorEvent{execution}, accounts);
}

}  // namespace

TEST(SpoofingLayeringDetector, NameIsSpoofingLayeringDetector) {
    SpoofingLayeringDetector detector;
    EXPECT_EQ(detector.name(), "SpoofingLayeringDetector");
}

// T1: the textbook case -- a dominant order, cancelled quickly, right
// after the opposite side's best price moved favorably during its life.
TEST(SpoofingLayeringDetector, TextbookSpoofFiresWithHighScore) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("A1", "ACC-X", Side::kSell, 101.00, 100, 1'000'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("S1", "SPOOFER", Side::kBuy, 99.00, 1000, 2'000'000'000),
                        accounts);  // sole order at 99.00 -> depth=1.0; opposite_best_at_placement=101.00
    apply_and_evaluate(book, detector, make_cancel("C1", "A1", 2'500'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("A2", "ACC-X", Side::kSell, 102.00, 100, 2'600'000'000),
                        accounts);  // best ask now 102.00 -- moved favorably for a resting Buy

    auto alerts = apply_and_evaluate(book, detector, make_cancel("C2", "S1", 2'700'000'000), accounts);
    // time_in_book = 700ms; speed = 1 - 0.7e9/5e9 = 0.86; depth = 1.0; move = 1.0
    // primary = (1 + 0.86 + 1) / 3 = 0.953333...
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_EQ(alerts[0].detector_name, "SpoofingLayeringDetector");
    EXPECT_NEAR(alerts[0].score, 0.953333333, 1e-6);
    EXPECT_EQ(alerts[0].instrument_id, kInstrument);
    ASSERT_EQ(alerts[0].account_ids.size(), 1u);
    EXPECT_EQ(alerts[0].account_ids[0], "SPOOFER");
    ASSERT_EQ(alerts[0].order_ids.size(), 1u);
    EXPECT_EQ(alerts[0].order_ids[0], "S1");
    EXPECT_EQ(alerts[0].window_start_ns, 2'000'000'000);
    EXPECT_EQ(alerts[0].window_end_ns, 2'700'000'000);
}

// T2: a genuinely large order, held a long time, with no price move --
// patient resting liquidity, not spoofing.
TEST(SpoofingLayeringDetector, GenuinelyPatientLargeOrderWithNoPriceMoveDoesNotFire) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("S1", "TRADER", Side::kBuy, 99.00, 1000, 1'000'000'000), accounts);
    // depth=1.0, but time_in_book = 10s (>> 5s slow threshold) -> speed=0; no asks ever -> move=0
    auto alerts = apply_and_evaluate(book, detector, make_cancel("C1", "S1", 11'000'000'000), accounts);
    // primary = (1 + 0 + 0) / 3 = 0.333
    EXPECT_TRUE(alerts.empty());
}

// T3: a small order, cancelled fast, but no price move -- fast cancels
// alone (common in legitimate flow) shouldn't be enough.
TEST(SpoofingLayeringDetector, SmallOrderFastCancelWithoutPriceMoveDoesNotFire) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("B1", "ACC-BASE", Side::kBuy, 99.00, 900, 500'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("S1", "SPOOFER", Side::kBuy, 99.00, 100, 1'000'000'000),
                        accounts);  // depth = 100/1000 = 0.1
    auto alerts = apply_and_evaluate(book, detector, make_cancel("C1", "S1", 1'100'000'000), accounts);
    // speed = 1 - 0.1e9/5e9 = 0.98; move = 0
    // primary = (0.1 + 0.98 + 0) / 3 = 0.36
    EXPECT_TRUE(alerts.empty());
}

// T4: a dominant order cancelled almost instantly fires even with no
// observed price move -- 2 of 3 strong signals clear the default
// threshold on their own. This is a deliberate, documented threshold
// property, not an accident (see class comment).
TEST(SpoofingLayeringDetector, DominantOrderCancelledVeryFastFiresEvenWithoutObservedPriceMove) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("S1", "SPOOFER", Side::kBuy, 99.00, 1000, 1'000'000'000), accounts);
    auto alerts = apply_and_evaluate(book, detector, make_cancel("C1", "S1", 1'050'000'000), accounts);
    // depth=1.0; speed = 1 - 0.05e9/5e9 = 0.99; move=0
    // primary = (1 + 0.99 + 0) / 3 = 0.663333...
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_NEAR(alerts[0].score, 0.663333333, 1e-6);
}

// T5a: moderate signals on their own, no layering -- stays under threshold.
TEST(SpoofingLayeringDetector, ModerateSignalsAloneWithoutLayeringDoNotFire) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("BASE", "ACC-BASE", Side::kBuy, 99.00, 300, 400'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("A1", "ACC-X", Side::kSell, 101.00, 100, 500'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("S1", "SPOOFER", Side::kBuy, 99.00, 100, 1'000'000'000),
                        accounts);  // depth = 100/400 = 0.25; opposite_best_at_placement = 101.00
    apply_and_evaluate(book, detector, make_cancel("C1", "A1", 2'000'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("A2", "ACC-X", Side::kSell, 103.00, 100, 2'100'000'000),
                        accounts);  // best ask now 103.00 -- moved favorably

    auto alerts = apply_and_evaluate(book, detector, make_cancel("C2", "S1", 3'500'000'000), accounts);
    // time_in_book = 2.5s; speed = 1 - 2.5/5 = 0.5; depth = 0.25; move = 1.0
    // primary = (0.25 + 0.5 + 1) / 3 = 0.583333...  (no layering) -- below default threshold 0.6
    EXPECT_TRUE(alerts.empty());
}

// T5b: identical scenario to T5a, but the same account also has 3 other
// concurrently-resting orders on the same side -- the layering bonus is
// what pushes an otherwise-subthreshold pattern over the bar. This is the
// test that actually exercises "Layering" in the detector's name, not just
// the three explicitly-named signals.
TEST(SpoofingLayeringDetector, ModerateSignalsWithConcurrentLayeredOrdersFires) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("BASE", "ACC-BASE", Side::kBuy, 99.00, 300, 400'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("A1", "ACC-X", Side::kSell, 101.00, 100, 500'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("S1", "SPOOFER", Side::kBuy, 99.00, 100, 1'000'000'000), accounts);
    // Three more concurrently-resting SPOOFER orders on the same side, at
    // other price levels -- don't affect S1's own depth_ratio (different
    // levels), only the layering count.
    apply_and_evaluate(book, detector, make_new("L1", "SPOOFER", Side::kBuy, 98.50, 50, 1'200'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("L2", "SPOOFER", Side::kBuy, 98.00, 50, 1'300'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("L3", "SPOOFER", Side::kBuy, 97.50, 50, 1'400'000'000), accounts);
    apply_and_evaluate(book, detector, make_cancel("C1", "A1", 2'000'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("A2", "ACC-X", Side::kSell, 103.00, 100, 2'100'000'000), accounts);

    auto alerts = apply_and_evaluate(book, detector, make_cancel("C2", "S1", 3'500'000'000), accounts);
    // primary = 0.583333 (as in T5a) + layering_bonus(0.15 * 1.0, 3 concurrent / saturation 3) = 0.733333...
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_NEAR(alerts[0].score, 0.733333333, 1e-6);
}

// T6: a fully-executed order never fires, even if a stray cancel arrives
// afterward -- letting an order trade is the opposite of spoofing.
TEST(SpoofingLayeringDetector, FullyExecutedOrderNeverFiresEvenIfStrayCancelFollows) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("S1", "SPOOFER", Side::kBuy, 99.00, 1000, 1'000'000'000), accounts);
    auto exec_alerts = apply_and_evaluate(book, detector, make_execution("S1", 1000, 1'050'000'000), accounts);
    EXPECT_TRUE(exec_alerts.empty());

    // OrderBook itself would silently no-op a cancel of an already-fully-
    // filled order (Phase 4 behavior) -- apply it directly to the detector
    // without re-applying to the book, since there's nothing left in the
    // book to apply it to; the detector's own tracked_ state (already
    // cleared by the full fill) is what this test is actually checking.
    auto cancel_alerts = detector.evaluate(book, DetectorEvent{make_cancel("C1", "S1", 1'100'000'000)}, accounts);
    EXPECT_TRUE(cancel_alerts.empty());
}

// Regression test for a real bug found via the Phase 9 live dashboard: a
// cancel timestamped *before* the New it's cancelling (only reachable if
// the underlying event stream goes non-monotonic -- a dropped New under
// cpp/ingestion/'s drop-oldest backpressure policy leaving tracked_'s
// entry stale, or cpp/api/main.cpp's demo feed restarting its synthetic
// clock and reusing an order_id across sessions) must never fire an
// alert. Before the fix, time_in_book_ns went negative and speed_score
// clamped to 1.0 -- "maximally fast", exactly backwards for what was
// actually corrupt timing data, not a genuine fast cancel.
TEST(SpoofingLayeringDetector, CancelTimestampedBeforeItsOwnPlacementNeverFires) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("S1", "SPOOFER", Side::kBuy, 99.00, 1000, 5'000'000'000), accounts);
    auto alerts = apply_and_evaluate(book, detector, make_cancel("C1", "S1", 1'000'000'000), accounts);

    EXPECT_TRUE(alerts.empty());
}

// The guard's boundary: a cancel timestamped *exactly* at its own
// placement time (order.timestamp_ns == tracked.placed_ts, zero-duration
// -- the most extreme genuinely-legitimate case, not an inverted one)
// must still fire normally, with time_in_book_ns exactly 0 and speed_score
// correctly clamped to 1.0. The fix is a strict `<` specifically so this
// boundary isn't swept in alongside genuinely-inverted (`<`) timestamps.
TEST(SpoofingLayeringDetector, CancelExactlyAtPlacementTimeStillFiresWithZeroDuration) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("S1", "SPOOFER", Side::kBuy, 99.00, 1000, 1'000'000'000), accounts);
    auto alerts = apply_and_evaluate(book, detector, make_cancel("C1", "S1", 1'000'000'000), accounts);

    // depth=1.0; speed = 1 - 0/5e9 = 1.0; move=0 -- primary = (1+1+0)/3 = 0.6667
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_NEAR(alerts[0].score, 0.666666667, 1e-6);
    EXPECT_NE(alerts[0].evidence.find("time_in_book_ns=0"), std::string::npos);
}

// T7: a cancel referencing an order this detector never saw a New for --
// silently ignored, not a crash, not a spurious alert.
TEST(SpoofingLayeringDetector, CancelOfUntrackedOrderIsIgnored) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    auto alerts =
        apply_and_evaluate(book, detector, make_cancel("C1", "NEVER_TRACKED", 1'000'000'000), accounts);
    EXPECT_TRUE(alerts.empty());
}

// T8: a Replace must re-base the tracked lifecycle onto the NEW order_id's
// own placement time and depth, not carry over the original order's stale
// values. Deliberately sets up different depth ratios at the old vs. new
// price level (0.25 vs 1.0) and different time-in-book results depending
// on which placed_ts is used (100ms vs 600ms), so a bug that failed to
// re-base would produce a detectably different score.
TEST(SpoofingLayeringDetector, ReplaceRebasesLifecycleOntoNewOrderIdentity) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("BASE", "ACC-BASE", Side::kBuy, 99.00, 3000, 500'000'000),
                        accounts);
    apply_and_evaluate(book, detector, make_new("O1", "SPOOFER", Side::kBuy, 99.00, 1000, 1'000'000'000),
                        accounts);  // depth if (wrongly) kept = 1000/4000 = 0.25
    apply_and_evaluate(
        book, detector,
        make_replace("R1", "O1", "SPOOFER", Side::kBuy, 98.00, 1000, 1'500'000'000), accounts);  // moves to a
                                                                                                    // fresh, empty
                                                                                                    // level -> depth
                                                                                                    // = 1.0

    auto alerts = apply_and_evaluate(book, detector, make_cancel("C1", "R1", 1'600'000'000), accounts);
    // Correct (rebased): placed_ts=1.5e9, time_in_book=100ms, speed=0.98; depth=1.0; move=0 (no asks)
    // primary = (1 + 0.98 + 0) / 3 = 0.66
    // A bug reusing O1's stale placed_ts (1.0e9) would give time_in_book=600ms, speed=0.88, primary=0.626667
    // A bug reusing O1's stale depth (0.25) would give primary substantially lower still.
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_NEAR(alerts[0].score, 0.66, 1e-6);
    EXPECT_EQ(alerts[0].order_ids[0], "R1");  // not "O1" -- old identity is fully retired
    EXPECT_EQ(alerts[0].window_start_ns, 1'500'000'000);  // the replace's own timestamp, not the original New's
}

// T9: layering count must only reflect the SAME account's other resting
// orders -- other accounts' concurrent orders at other levels are just
// normal market depth, not evidence of this account layering.
TEST(SpoofingLayeringDetector, LayeringCountExcludesOtherAccountsConcurrentOrders) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("S1", "SPOOFER", Side::kBuy, 99.00, 1000, 1'000'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("O1", "OTHER-ACC", Side::kBuy, 98.50, 50, 1'100'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("O2", "OTHER-ACC", Side::kBuy, 98.00, 50, 1'200'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("O3", "OTHER-ACC", Side::kBuy, 97.50, 50, 1'300'000'000), accounts);

    auto alerts = apply_and_evaluate(book, detector, make_cancel("C1", "S1", 1'050'000'000), accounts);
    // Same primary as DominantOrderCancelledVeryFastFiresEvenWithoutObservedPriceMove (0.663333...) --
    // if OTHER-ACC's orders were wrongly counted as SPOOFER's layering, this would instead be 0.813333.
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_NEAR(alerts[0].score, 0.663333333, 1e-6);
}

// A lower alert_threshold genuinely changes the firing decision -- proves
// the config is load-bearing, not decorative (relevant for Phase 10's
// planned threshold sweep).
TEST(SpoofingLayeringDetector, CustomLowerThresholdFiresOnTheOtherwiseSubthresholdCase) {
    OrderBook book(kInstrument);
    SpoofingLayeringConfig config;
    config.alert_threshold = 0.5;
    SpoofingLayeringDetector detector(config);
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("BASE", "ACC-BASE", Side::kBuy, 99.00, 300, 400'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("A1", "ACC-X", Side::kSell, 101.00, 100, 500'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("S1", "SPOOFER", Side::kBuy, 99.00, 100, 1'000'000'000), accounts);
    apply_and_evaluate(book, detector, make_cancel("C1", "A1", 2'000'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("A2", "ACC-X", Side::kSell, 103.00, 100, 2'100'000'000), accounts);

    auto alerts = apply_and_evaluate(book, detector, make_cancel("C2", "S1", 3'500'000'000), accounts);
    // Same 0.583333 primary as ModerateSignalsAloneWithoutLayeringDoNotFire, which doesn't fire at the
    // default 0.6 threshold -- but does at 0.5.
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_NEAR(alerts[0].score, 0.583333333, 1e-6);
}
