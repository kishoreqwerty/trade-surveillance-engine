#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "i_detector.hpp"

namespace tse::detectors {

struct SpoofingLayeringConfig {
    double alert_threshold{0.6};

    // A cancel at or beyond this time-in-book scores 0 on the speed
    // component; a cancel at time 0 scores 1. Linear in between.
    int64_t slow_time_in_book_ns{5'000'000'000LL};  // 5s

    // Minimum absolute price change on the opposite side, over the
    // tracked order's lifetime, to count as "the market moved" rather than
    // noise.
    double min_opposite_price_move{0.01};

    // Number of the same account's other concurrently-resting orders (same
    // instrument, same side) at which the layering signal saturates to 1.0.
    int layering_saturation_count{3};

    // Layering is additive, not part of the primary three-signal average —
    // see the class comment for why.
    double layering_bonus_weight{0.15};
};

// The highest-scrutiny detector in this phase (CLAUDE.md: "do not
// under-test the order book or this detector relative to other modules").
// Scores three signals named directly in the build guide, plus one more
// this class's own name calls for:
//
//   1. depth_score  — this order's share of the visible depth *at its own
//      price level*, captured the instant it's inserted (order.qty /
//      qty_at_price(side, price) right after New is applied — the level
//      necessarily includes this order already, since evaluate() runs
//      after OrderBook has applied the update). Per-level, not per-side-
//      total: the suspicious quantity is "how much of *this specific
//      queue* would move if this order weren't there," which is what an
//      order actually dominating a level captures; total-side depth would
//      dilute that signal across price levels this order has no influence
//      over.
//
//   2. speed_score   — how quickly the order was pulled after resting,
//      linear from 1.0 (cancelled instantly) to 0.0 (cancelled at or past
//      slow_time_in_book_ns). A fast cancel alone isn't damning (plenty of
//      legitimate order flow cancels quickly), but combined with the other
//      signals it's a real tell: spoofing only works if the order is
//      pulled before it risks executing.
//
//   3. move_score    — did the OPPOSITE side's best price move favorably
//      (for a resting Buy: opposite/ask best price rose; for a resting
//      Sell: opposite/bid best price fell) between this order's placement
//      and its cancellation, by at least min_opposite_price_move? Uses the
//      *opposite* side deliberately, not the same side: removing a resting
//      order can never itself move the opposite side's best price, so this
//      signal can't be self-referentially distorted by the very cancel
//      being evaluated (an earlier same-side design was rejected for
//      exactly this reason — see cpp/detectors/README.md). This also
//      matches the real mechanism layering is accused of: a large resting
//      order creates a false impression of supply/demand that shifts what
//      the *other* side is willing to trade at, and the spoofer cancels
//      once that shift has happened rather than ever risking a fill.
//
//   4. layering_score — how many *other* orders the same account currently
//      has resting on the same side of the same instrument, saturating at
//      layering_saturation_count. This is what actually engages with
//      "Layering" in the class name, beyond the three explicitly-named
//      signals: a single spoofed order is "spoofing"; several at once
//      across the book is "layering." Deliberately additive
//      (layering_bonus_weight * layering_score, not folded into the
//      averaged primary three) rather than a fourth equally-weighted
//      signal — layering is a compounding aggravator of an
//      already-suspicious single-order pattern, not an independent
//      signal that should let a textbook non-spoof order fire purely
//      because the same account happens to have other legitimate resting
//      orders elsewhere.
//
// combined = clamp((depth_score + speed_score + move_score) / 3
//                   + layering_bonus_weight * layering_score, 0, 1)
// Fires (returns one Alert) iff combined >= alert_threshold.
//
// State: a per-order-id map tracking each resting order's placement
// snapshot (account, instrument, side, price, remaining qty,
// depth_score's inputs, opposite-side best price at placement), built on
// New, consulted and erased on Cancel, decremented on Execution (reaching
// zero removes it — a fully-executed order isn't spoofing, it's genuine
// flow), and re-based on Replace (the old order_id's entry is retired
// without emitting anything, and the new order_id starts an entirely
// fresh lifecycle — a Replace mints a new identity per Phase 4's
// order_book.hpp, and re-using the original placement snapshot across a
// price/qty amendment would conflate two different resting postures).
class SpoofingLayeringDetector : public IDetector {
public:
    explicit SpoofingLayeringDetector(SpoofingLayeringConfig config = {});

    std::vector<Alert> evaluate(const tse::orderbook::OrderBook& book, const DetectorEvent& incoming,
                                 const AccountRegistry& accounts) override;

    std::string name() const override { return "SpoofingLayeringDetector"; }

private:
    struct TrackedOrder {
        std::string account_id;
        std::string instrument_id;
        tse::fix::Side side{tse::fix::Side::kBuy};
        int64_t remaining_qty{0};
        int64_t placed_ts{0};
        double depth_ratio_at_placement{0.0};
        std::optional<double> opposite_best_at_placement;
    };

    void track_new(const tse::orderbook::OrderBook& book, const tse::fix::Order& order);
    void handle_replace(const tse::orderbook::OrderBook& book, const tse::fix::Order& order);
    void handle_execution(const tse::fix::Execution& execution);
    std::vector<Alert> handle_cancel(const tse::orderbook::OrderBook& book, const tse::fix::Order& order);
    int count_concurrent_same_account_same_side(const std::string& account_id, const std::string& instrument_id,
                                                 tse::fix::Side side) const;

    SpoofingLayeringConfig config_;
    std::unordered_map<std::string, TrackedOrder> tracked_;
};

}  // namespace tse::detectors
