#include "spoofing_layering_detector.hpp"

#include <string>

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
    // time_in_book = 700ms; speed = 1 - 0.7e9/5e9 = 0.86; depth = 1.0.
    // move (Phase 11.5, density-normalized): A2's placement at t=2.6s is
    // the only recorded ask-side move in the density window (30s), so
    // recent_move_rate = 1/30/sec; expected_moves_during_dwell =
    // (1/30)*0.7 = 7/300; move = 1 - 7/300 = 293/300 (not a flat 1.0 --
    // the single move that makes moved_favorably=true is itself counted
    // as one ambient observation, a deliberate, small, accepted effect of
    // the redesign, not a bug -- see PARAMETER_MAPPING.md).
    // primary = (1 + 0.86 + 293/300) / 3 = 851/900 = 0.945555...
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_EQ(alerts[0].detector_name, "SpoofingLayeringDetector");
    EXPECT_NEAR(alerts[0].score, 851.0 / 900.0, 1e-9);
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
    // time_in_book = 2.5s; speed = 1 - 2.5/5 = 0.5; depth = 0.25.
    // move (Phase 11.5, density-normalized): one ask-side move recorded
    // (A2 at t=2.1s); expected_moves_during_dwell = (1/30)*2.5 = 1/12;
    // move = 11/12 (not a flat 1.0 -- see
    // CustomLowerThresholdFiresOnTheOtherwiseSubthresholdCase for the
    // full derivation of this same setup).
    // primary = (0.25 + 0.5 + 11/12) / 3 = 5/9 = 0.555555...  (no
    // layering) -- below default threshold 0.6 either way.
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
    // depth = 0.25 (100 / (300 BASE + 100 S1) at S1's own price level); speed = 1 - 2.5e9/5e9 = 0.5.
    // move (Phase 11.5): one ask-side move recorded (A2 at t=2.1s); dwell
    // = 2.5s; expected_moves_during_dwell = (1/30)*2.5 = 1/12; move = 11/12.
    // primary = (0.25 + 0.5 + 11/12) / 3 = 5/9.
    // layering (Phase 11.5): concurrent=3 (L1/L2/L3, S1 itself just
    // erased); typical_concurrent excludes SPOOFER's own samples --
    // averaged only over ACC-BASE's one buy-side sample (count=1) -- so
    // typical_concurrent=1, not 0. layering = clamp((3-1)/3, 0, 1) = 2/3.
    // combined = 5/9 + 0.15*(2/3) = 59/90 = 0.655555...
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_NEAR(alerts[0].score, 59.0 / 90.0, 1e-9);
}

// Phase 11.5 regression: a real bug found and fixed while implementing
// the density-normalization above, not a hypothetical. The ambient
// "typical concurrent count" baseline for layering_score must exclude
// the tracked order's OWN account -- otherwise an account's own layering
// pattern is the dominant (here, the ONLY) contributor to what counts as
// "typical for anyone right now," silently normalizing away the very
// pattern being evaluated. This scenario has SPOOFER as the only account
// on this side/instrument at all (no ACC-BASE-style other participant),
// so without the self-exclusion, typical_concurrent would be computed
// from SPOOFER's own history (average of 1,2,3 = 2), suppressing
// layering_score toward 0 for a textbook 3-deep layering pattern.
TEST(SpoofingLayeringDetector, AmbientLayeringBaselineExcludesTrackedOrdersOwnAccount) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("S1", "SPOOFER", Side::kBuy, 99.00, 1000, 1'000'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("L1", "SPOOFER", Side::kBuy, 98.50, 50, 1'100'000'000), accounts);
    apply_and_evaluate(book, detector, make_new("L2", "SPOOFER", Side::kBuy, 98.00, 50, 1'200'000'000), accounts);

    auto alerts = apply_and_evaluate(book, detector, make_cancel("C1", "S1", 1'250'000'000), accounts);
    // depth = 1.0 (sole order at its own price level); speed = 1 - 0.25e9/5e9 = 0.95; move = 0 (no opposite-side
    // activity at all). concurrent = 2 (L1, L2, after S1 erased). With NO other account ever seen on this
    // side/instrument, typical_concurrent must be 0 (nothing to average, not SPOOFER's own 1/2/3 history) --
    // layering = clamp((2-0)/3, 0, 1) = 2/3, matching the ORIGINAL (pre-Phase-11.5) formula exactly in this
    // specific case, since there's genuinely no ambient data to normalize against.
    // primary = (1.0 + 0.95 + 0) / 3 = 0.65; combined = 0.65 + 0.15*(2/3) = 0.75.
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_NEAR(alerts[0].score, 0.75, 1e-9);
}

// Phase 11.5 regression: a real measured regression found via the Sarao
// validation case, not a hypothetical. Before capping effective_dwell_s
// at slow_time_in_book_ns, a long-dwelling order (Sarao's real,
// historically-documented pattern dwells ~8s) took a move_score discount
// that grew unboundedly with its own dwell length, even though
// speed_score is already floored at 0 past slow_time_in_book_ns (5s) --
// double-penalizing the same "not fast" fact through two channels.
// Mirrors Sarao's exact shape: one ask-side move recorded, then an 8s
// dwell (longer than slow_time_in_book_ns) before cancelling.
TEST(SpoofingLayeringDetector, MoveScoreDwellDiscountCappedAtSlowTimeInBookNsNotFullDwell) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("A1", "ACC-X", Side::kSell, 101.00, 100, 0), accounts);
    apply_and_evaluate(book, detector, make_new("S1", "SPOOFER", Side::kBuy, 99.00, 1000, 1'000'000'000),
                        accounts);  // sole order at 99.00 -> depth=1.0; opposite_best_at_placement=101.00
    apply_and_evaluate(book, detector, make_cancel("CA1", "A1", 1'500'000'000), accounts);  // clears the way for A2
    apply_and_evaluate(book, detector, make_new("A2", "ACC-X", Side::kSell, 102.00, 100, 2'000'000'000),
                        accounts);  // best ask now 102.00 -- the one ask-side move recorded in the density window

    auto alerts = apply_and_evaluate(book, detector, make_cancel("C1", "S1", 9'000'000'000), accounts);
    // time_in_book = 8s (> slow_time_in_book_ns=5s) -> speed_score=0, same as Sarao.
    // move: effective_dwell_s = min(8, 5) = 5 (capped); expected_moves_during_dwell = (1/30)*5 = 1/6;
    // move = 5/6. primary = (1 + 0 + 5/6) / 3 = 11/18 = 0.611... -- clears the default 0.6 threshold.
    // Uncapped, this would have been effective_dwell_s=8, expected_moves=(1/30)*8=4/15, move=11/15,
    // primary=(1+0+11/15)/3=26/45=0.5778 -- BELOW threshold. The cap is what makes the difference here.
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_NEAR(alerts[0].score, 11.0 / 18.0, 1e-9);
}

// Regression for the alert-persistence-stall investigation (see
// cpp/pipeline/README.md): update_ambient()'s prune step used to be a
// front-only trim (`while front too old, pop_front`), which silently
// assumes event_ts is monotonically non-decreasing across calls. The live
// demo server violates that assumption for real: main.cpp's feeder loop
// reuses the same fixed session_start_ns anchor every session, so the
// live event_ts stream genuinely regresses backward at every session
// boundary. Under the old trim, a regressed event_ts compared against a
// deque whose front is a much-later timestamp never satisfies
// `event_ts - front > density_window_ns` (the subtraction goes negative),
// so nothing gets pruned -- stale entries from a "future" session stay
// stuck in the deque forever, inflating recent_move_rate and permanently
// suppressing move_score. This test proves the fix (symmetric
// std::llabs()-based pruning, mirroring FrontRunningDetector's own
// already-robust recent_by_key_) correctly discards those stale entries
// instead.
TEST(SpoofingLayeringDetector, AmbientPruneSurvivesATimestampRegressionAcrossASessionBoundary) {
    OrderBook book(kInstrument);
    SpoofingLayeringDetector detector;
    AccountRegistry accounts;

    apply_and_evaluate(book, detector, make_new("SEED-ASK", "SEEDACCT", Side::kSell, 100.00, 10, 0), accounts);

    // "Session 1": 10 ask-side moves, each strictly better than the last,
    // clustered at t=110s..200s -- far in the future relative to the
    // "session 2" timestamps below, exactly like one real demo session's
    // worth of events landing near the end of its own 90s window before
    // the next session restarts the clock.
    for (int i = 1; i <= 10; ++i) {
        apply_and_evaluate(
            book, detector,
            make_new("STALE-" + std::to_string(i), "STALEACCT", Side::kSell, 100.00 - i, 10, (100 + 10 * i) * 1'000'000'000LL),
            accounts);
    }
    // Best ask is now 90.00 (STALE-10).

    // "Session 2" restarts at t=1s -- a genuine backward regression from
    // the 110-200s cluster above, exactly like main.cpp's feeder looping
    // back to its fixed shared anchor.
    apply_and_evaluate(book, detector, make_new("S1", "SPOOFER", Side::kBuy, 50.00, 1000, 1'000'000'000),
                        accounts);  // sole order at its price -> depth=1.0; opposite_best_at_placement=90.00 (STALE-10)

    // The one genuine "session 2" ask move: cancelling STALE-10 (currently
    // the best ask) reveals STALE-9 at 91.00 as the new best -- a real
    // rise, satisfying moved_favorably for S1's resting Buy.
    apply_and_evaluate(book, detector, make_cancel("C-STALE-10", "STALE-10", 1'500'000'000), accounts);

    auto alerts = apply_and_evaluate(book, detector, make_cancel("C1", "S1", 2'000'000'000), accounts);
    // dwell = 1s -> speed = 1 - 1/5 = 0.8. moved_favorably: 91.00 - 90.00 = +1.00 >= 0.01 -> true.
    //
    // Correct (fixed) pruning: at scoring time (t=2.0s), all 10 stale
    // entries (110-200s) are 108-198s away from "now" -- far outside the
    // 30s density window on EITHER side of a regression, so all 10 are
    // pruned regardless of direction. Only the t=1.5s entry survives
    // (0.5s away). recent_move_rate = 1/30 per second.
    // expected_moves_during_dwell = (1/30)*1 = 1/30. move = 1 - 1/30 = 29/30.
    // primary = (1 + 0.8 + 29/30) / 3 = (30+24+29)/90 = 83/90.
    // layering: S1 is the only account ever on the Buy side, so
    // typical_concurrent=0 (nothing to average) and concurrent=0 (S1
    // itself just erased) -> layering=0. combined = 83/90.
    //
    // Verified empirically against the OLD front-only trim (temporarily
    // reverted, this test run, then restored): it does NOT get stuck at
    // 11 entries. Front-only trimming still works correctly *within* a
    // monotonically-increasing run -- while processing the stale phase's
    // own t=110..200s events in order, the old trim legitimately pops
    // 110/120/130/140/150/160 (each >30s behind the *then-current*
    // event_ts), settling to [170,180,190,200] (4 entries) before the
    // regression ever happens. It's specifically the regression boundary
    // that breaks it: front stays at 170s forever after (event_ts(~1-2s)
    // - 170s is always negative, never > 30s), so those 4 plus the one
    // real post-regression entry (5 total) get stuck permanently.
    // recent_move_rate = 5/30, expected_moves = 5/30, move = 25/30 = 5/6,
    // primary = (30+24+25)/90 = 79/90 -- a real, measurably wrong
    // (too-low) score this test catches (confirmed: fails with 0.878
    // against 83/90 when run against the old code).
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_NEAR(alerts[0].score, 83.0 / 90.0, 1e-9);
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
    // Same setup and same density-normalized move_score derivation as
    // ModerateSignalsWithConcurrentLayeredOrdersFires above (no L1-L3
    // here, so no layering contribution): primary = 5/9 = 0.555555...,
    // below the default 0.6 threshold -- but fires at this test's custom 0.5.
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_NEAR(alerts[0].score, 5.0 / 9.0, 1e-9);
}
