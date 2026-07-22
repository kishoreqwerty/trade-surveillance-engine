#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

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

    // Phase 11.5: rolling window used to density-normalize move_score and
    // layering_score against RECENT ambient conditions instead of the
    // fixed absolute constants above alone (min_opposite_price_move,
    // layering_saturation_count) -- both were found, via a real order-rate
    // recalibration (Row 6), to be structurally more likely to cross their
    // bar by chance alone as ambient order-flow density rose (see
    // PARAMETER_MAPPING.md). 30s = 6x slow_time_in_book_ns, not an
    // independent constant: move_score's comparison is inherently scaled
    // by an order's own dwell time (bounded by slow_time_in_book_ns, since
    // slower cancels already score 0 on speed regardless), so the ambient
    // window needs to span several dwell-periods' worth of potential
    // events for the rate estimate to not be dominated by single-
    // observation noise, while staying short enough to reflect genuinely
    // recent conditions rather than session-wide history. Deliberately
    // NOT anchored to any simulator-side calibrated rate (e.g. Row 6's
    // per-instrument order rate): detectors/ must not depend on
    // simulator/, and this detector also has to work against real/
    // replayed data (the Sarao case) where no synthetic calibration
    // applies at all.
    int64_t density_window_ns{30'000'000'000LL};  // 30s = 6 * slow_time_in_book_ns's own default
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
//      and its cancellation? Uses the *opposite* side deliberately, not
//      the same side: removing a resting order can never itself move the
//      opposite side's best price, so this signal can't be self-
//      referentially distorted by the very cancel being evaluated (an
//      earlier same-side design was rejected for exactly this reason —
//      see cpp/detectors/README.md). This also matches the real mechanism
//      layering is accused of: a large resting order creates a false
//      impression of supply/demand that shifts what the *other* side is
//      willing to trade at, and the spoofer cancels once that shift has
//      happened rather than ever risking a fill.
//
//      Phase 11.5: density-normalized, not a flat 1.0/0.0 against a fixed
//      min_opposite_price_move. A real order-rate recalibration (Row 6)
//      found this signal structurally more likely to trip by chance alone
//      as ambient order-flow density rose — at high density the opposite
//      touch ticks often regardless of any specific order's own behavior.
//      Scored as clamp(1 - expected_moves_during_dwell, 0, 1) when a move
//      occurred (0 otherwise), where expected_moves_during_dwell is this
//      order's own dwell duration (capped at slow_time_in_book_ns -- past
//      that point speed_score is already at its floor, so extrapolating
//      this discount further out compounds the same "not fast" signal a
//      second time rather than adding new information; found necessary
//      via a real measured regression in the Sarao validation case's
//      long, ~8s historically-documented dwells) times a rolling recent
//      move-rate for that side (density_window_ns, see config). A move
//      that ambient churn alone would statistically produce during a
//      dwell this long isn't evidence of this specific order's influence;
//      a move in an
//      otherwise-quiet market is fully suspicious, same as before.
//
//   4. layering_score — how many *other* orders the same account currently
//      has resting on the same side of the same instrument, relative to a
//      rolling ambient baseline (see below), saturating at
//      layering_saturation_count above that baseline. This is what
//      actually engages with "Layering" in the class name, beyond the
//      three explicitly-named signals: a single spoofed order is
//      "spoofing"; several at once across the book is "layering."
//      Deliberately additive (layering_bonus_weight * layering_score, not
//      folded into the averaged primary three) rather than a fourth
//      equally-weighted signal — layering is a compounding aggravator of
//      an already-suspicious single-order pattern, not an independent
//      signal that should let a textbook non-spoof order fire purely
//      because the same account happens to have other legitimate resting
//      orders elsewhere.
//
//      Phase 11.5: density-normalized, not a flat ratio against a fixed
//      layering_saturation_count alone — the same Row 6 recalibration
//      found any account more likely to coincidentally have multiple
//      orders open at once purely from higher ambient volume, independent
//      of genuine layering. Scored as
//      clamp((concurrent - typical_concurrent) / layering_saturation_count, 0, 1),
//      where typical_concurrent is a rolling average of concurrent-order-
//      count samples pooled across *all* accounts on that instrument+side
//      (density_window_ns) — "what's normal for anyone right now," not
//      this account's own history. At low ambient density typical_concurrent
//      ≈ 0 and this reduces to the original formula.
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

    // Rolling ambient conditions for one (instrument, side), used to
    // density-normalize move_score/layering_score -- see the class
    // comment. recent_move_timestamps: this side's best-price change
    // events. recent_concurrent_samples: (timestamp, account_id,
    // concurrent-count) tuples recorded on every track_new(), pooled
    // across all accounts -- account_id is kept specifically so a
    // tracked order's OWN account can be excluded when computing ITS OWN
    // "typical" baseline (found necessary: without this, an account's own
    // layering pattern was the dominant contributor to what counted as
    // "typical for anyone right now," partially normalizing away the very
    // pattern being evaluated -- a real bug found by a failing existing
    // test, not a hypothetical). Both deques trimmed to density_window_ns
    // on every update. O(1) amortized per event (each entry pushed once,
    // popped once) -- this detector is
    // the project's documented hot path (see concurrent_count_by_key_'s
    // own comment on a past latency regression here), so this was sized
    // deliberately to avoid reintroducing one.
    struct ConcurrentSample {
        int64_t timestamp_ns{0};
        std::string account_id;
        int concurrent_count{0};
    };

    struct AmbientTracker {
        std::deque<int64_t> recent_move_timestamps;
        std::deque<ConcurrentSample> recent_concurrent_samples;
        std::optional<double> last_best_price;
    };

    void track_new(const tse::orderbook::OrderBook& book, const tse::fix::Order& order);
    void handle_replace(const tse::orderbook::OrderBook& book, const tse::fix::Order& order);
    void handle_execution(const tse::fix::Execution& execution);
    std::vector<Alert> handle_cancel(const tse::orderbook::OrderBook& book, const tse::fix::Order& order);

    // Called once per incoming event, for both sides of instrument_id,
    // before any scoring logic runs -- keeps ambient_by_instrument_side_
    // current regardless of which event type triggered it, since any
    // event (New/Cancel/Replace/Execution) can move a best price.
    void update_ambient(const tse::orderbook::OrderBook& book, const std::string& instrument_id, int64_t event_ts);

    // erase_tracked() removes tracked_[order_id] (if present) and keeps
    // concurrent_count_by_key_ in sync in the same step -- every erase of a
    // TrackedOrder must go through this, not tracked_.erase() directly, or
    // the two data structures drift apart. Returns the erased entry, or
    // std::nullopt if order_id wasn't tracked.
    std::optional<TrackedOrder> erase_tracked(const std::string& order_id);

    int count_concurrent_same_account_same_side(const std::string& account_id, const std::string& instrument_id,
                                                 tse::fix::Side side) const;

    SpoofingLayeringConfig config_;
    std::unordered_map<std::string, TrackedOrder> tracked_;

    // Key: instrument_id + side (pooled across accounts, deliberately not
    // per-account -- the point is "what's typical for anyone right now").
    std::unordered_map<std::string, AmbientTracker> ambient_by_instrument_side_;

    // Incrementally-maintained count of resting orders per (account,
    // instrument, side) -- what count_concurrent_same_account_same_side()
    // actually reads. Added after Phase 6's sustained-load measurement
    // showed the original implementation (a full linear scan over all of
    // tracked_ on every single Cancel) had a heavy latency tail: tracked_
    // can hold thousands of entries across a multi-hour, multi-instrument
    // session, and every one of those entries was being examined on every
    // cancel regardless of relevance. Key: account_id + "|" + instrument_id
    // + "|" + side. See track_new()/erase_tracked() for where this is kept
    // in sync -- every insertion/removal of a TrackedOrder must go through
    // exactly those two functions, never touch tracked_ directly elsewhere.
    std::unordered_map<std::string, int> concurrent_count_by_key_;
};

}  // namespace tse::detectors
