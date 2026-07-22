#include "spoofing_layering_detector.hpp"

#include <algorithm>

namespace tse::detectors {

using tse::fix::Execution;
using tse::fix::Order;
using tse::fix::OrderStatus;
using tse::fix::Side;
using tse::orderbook::OrderBook;

SpoofingLayeringDetector::SpoofingLayeringDetector(SpoofingLayeringConfig config) : config_(config) {}

namespace {
Side opposite_of(Side side) { return side == Side::kBuy ? Side::kSell : Side::kBuy; }

std::string concurrency_key(const std::string& account_id, const std::string& instrument_id, Side side) {
    return account_id + "|" + instrument_id + "|" + (side == Side::kBuy ? "B" : "S");
}

std::string instrument_side_key(const std::string& instrument_id, Side side) {
    return instrument_id + "|" + (side == Side::kBuy ? "B" : "S");
}
}  // namespace

void SpoofingLayeringDetector::update_ambient(const OrderBook& book, const std::string& instrument_id,
                                               int64_t event_ts) {
    for (Side side : {Side::kBuy, Side::kSell}) {
        AmbientTracker& tracker = ambient_by_instrument_side_[instrument_side_key(instrument_id, side)];
        const std::optional<double> price = book.best_price(side);
        if (tracker.last_best_price.has_value() && price.has_value() && *price != *tracker.last_best_price) {
            tracker.recent_move_timestamps.push_back(event_ts);
        }
        while (!tracker.recent_move_timestamps.empty() &&
               event_ts - tracker.recent_move_timestamps.front() > config_.density_window_ns) {
            tracker.recent_move_timestamps.pop_front();
        }
        while (!tracker.recent_concurrent_samples.empty() &&
               event_ts - tracker.recent_concurrent_samples.front().timestamp_ns > config_.density_window_ns) {
            tracker.recent_concurrent_samples.pop_front();
        }
        if (price.has_value()) tracker.last_best_price = price;
    }
}

void SpoofingLayeringDetector::track_new(const OrderBook& book, const Order& order) {
    TrackedOrder tracked;
    tracked.account_id = order.account_id;
    tracked.instrument_id = order.instrument_id;
    tracked.side = order.side;
    tracked.remaining_qty = order.qty;
    tracked.placed_ts = order.timestamp_ns;

    const int64_t level_qty = book.qty_at_price(order.side, order.price);  // includes this order already
    tracked.depth_ratio_at_placement =
        level_qty > 0 ? static_cast<double>(order.qty) / static_cast<double>(level_qty) : 0.0;
    tracked.opposite_best_at_placement = book.best_price(opposite_of(order.side));

    tracked_[order.order_id] = tracked;
    const int new_count = ++concurrent_count_by_key_[concurrency_key(tracked.account_id, tracked.instrument_id, tracked.side)];

    // Record this as an ambient sample of "what's a typical concurrent
    // count right now" -- pooled across accounts (account_id kept so a
    // tracked order's own account can be excluded from its own baseline
    // later, see AmbientTracker's comment).
    AmbientTracker& tracker = ambient_by_instrument_side_[instrument_side_key(order.instrument_id, order.side)];
    tracker.recent_concurrent_samples.push_back({order.timestamp_ns, order.account_id, new_count});
}

std::optional<SpoofingLayeringDetector::TrackedOrder> SpoofingLayeringDetector::erase_tracked(
    const std::string& order_id) {
    auto it = tracked_.find(order_id);
    if (it == tracked_.end()) return std::nullopt;
    TrackedOrder erased = it->second;
    tracked_.erase(it);
    const std::string key = concurrency_key(erased.account_id, erased.instrument_id, erased.side);
    auto count_it = concurrent_count_by_key_.find(key);
    if (count_it != concurrent_count_by_key_.end()) {
        if (--count_it->second <= 0) concurrent_count_by_key_.erase(count_it);
    }
    return erased;
}

void SpoofingLayeringDetector::handle_replace(const OrderBook& book, const Order& order) {
    erase_tracked(order.orig_order_id);  // old identity retired without emitting anything (see class comment)
    track_new(book, order);              // new identity starts a fresh lifecycle
}

void SpoofingLayeringDetector::handle_execution(const Execution& execution) {
    auto it = tracked_.find(execution.order_id);
    if (it == tracked_.end()) return;
    it->second.remaining_qty -= execution.qty;
    if (it->second.remaining_qty <= 0) {
        erase_tracked(execution.order_id);  // fully executed: genuine flow, not spoofing -- just stop tracking it
    }
}

int SpoofingLayeringDetector::count_concurrent_same_account_same_side(const std::string& account_id,
                                                                       const std::string& instrument_id,
                                                                       Side side) const {
    auto it = concurrent_count_by_key_.find(concurrency_key(account_id, instrument_id, side));
    return it == concurrent_count_by_key_.end() ? 0 : it->second;
}

std::vector<Alert> SpoofingLayeringDetector::handle_cancel(const OrderBook& book, const Order& order) {
    std::optional<TrackedOrder> maybe_tracked = erase_tracked(order.orig_order_id);
    if (!maybe_tracked.has_value()) return {};  // not an order this detector was tracking -- silently ignore
    const TrackedOrder tracked = *maybe_tracked;  // lifecycle complete either way

    // A cancel can never genuinely precede the placement it's cancelling.
    // If it appears to, tracked_'s entry for this order_id is stale --
    // most plausibly this order's own New was dropped by
    // cpp/ingestion/'s drop-oldest backpressure policy (see
    // cpp/ingestion/README.md), leaving behind a same-order_id entry from
    // an earlier, unrelated lifecycle (a multi-session replay/demo feed
    // reusing order_id ranges across restarted synthetic sessions --
    // cpp/api/main.cpp -- makes this concretely reachable, not just
    // theoretical). Bail out rather than compute a nonsensical negative
    // time_in_book_ns and let it silently inflate speed_score (clamped to
    // 1.0 -- "maximally fast" -- for what was actually corrupt timing
    // data, not a genuine fast cancel).
    if (order.timestamp_ns < tracked.placed_ts) return {};

    const int64_t time_in_book_ns = order.timestamp_ns - tracked.placed_ts;
    const double speed_score =
        config_.slow_time_in_book_ns > 0
            ? std::clamp(1.0 - static_cast<double>(time_in_book_ns) / static_cast<double>(config_.slow_time_in_book_ns),
                         0.0, 1.0)
            : 0.0;
    const double depth_score = std::clamp(tracked.depth_ratio_at_placement, 0.0, 1.0);

    bool moved_favorably = false;
    const std::optional<double> opposite_now = book.best_price(opposite_of(tracked.side));
    if (tracked.opposite_best_at_placement.has_value() && opposite_now.has_value()) {
        const double delta = *opposite_now - *tracked.opposite_best_at_placement;
        moved_favorably = tracked.side == Side::kBuy ? (delta >= config_.min_opposite_price_move)
                                                      : (delta <= -config_.min_opposite_price_move);
    }
    // Density-normalized (Phase 11.5): a move that recent ambient churn
    // alone would statistically produce during a dwell this long isn't
    // evidence of this specific order's influence -- see class comment.
    //
    // effective_dwell_s is capped at slow_time_in_book_ns, not the order's
    // full (possibly much longer) dwell: past that point speed_score is
    // already at its floor (0), so the detector has already signalled
    // "not fast" through that channel -- extrapolating the move-rate
    // discount further out adds no new information, it only compounds
    // the same signal a second time. Found necessary via the real Sarao
    // validation case: its historically-documented ~8s dwells (already
    // long enough to floor speed_score) were, before this cap, ALSO
    // taking a disproportionate move_score discount purely from dwell
    // length, dropping 5 of 25 previously-firing alerts below threshold
    // (the zero-concurrent-layering tier specifically) -- not a
    // hypothetical, a measured regression this cap fixes.
    double move_score = 0.0;
    if (moved_favorably) {
        const auto opp_it =
            ambient_by_instrument_side_.find(instrument_side_key(tracked.instrument_id, opposite_of(tracked.side)));
        double recent_move_rate = 0.0;  // moves/sec
        if (opp_it != ambient_by_instrument_side_.end() && config_.density_window_ns > 0) {
            const double window_s = static_cast<double>(config_.density_window_ns) / 1e9;
            recent_move_rate = static_cast<double>(opp_it->second.recent_move_timestamps.size()) / window_s;
        }
        const double dwell_s = static_cast<double>(time_in_book_ns) / 1e9;
        const double dwell_cap_s = static_cast<double>(config_.slow_time_in_book_ns) / 1e9;
        const double effective_dwell_s = std::min(dwell_s, dwell_cap_s);
        const double expected_moves_during_dwell = recent_move_rate * effective_dwell_s;
        move_score = std::clamp(1.0 - expected_moves_during_dwell, 0.0, 1.0);
    }

    const int concurrent = count_concurrent_same_account_same_side(tracked.account_id, tracked.instrument_id,
                                                                     tracked.side);
    // Density-normalized (Phase 11.5): subtract off "what's a typical
    // concurrent count for OTHER accounts right now" before comparing
    // against the saturation scale -- see class comment. Deliberately
    // excludes tracked.account_id's own samples: including them made an
    // account's own layering the dominant contributor to what counted as
    // "typical," partially normalizing away the very pattern being
    // evaluated (found via a failing existing test, not assumed safe).
    double typical_concurrent = 0.0;
    const auto same_side_it =
        ambient_by_instrument_side_.find(instrument_side_key(tracked.instrument_id, tracked.side));
    if (same_side_it != ambient_by_instrument_side_.end()) {
        double sum = 0.0;
        int count = 0;
        for (const auto& sample : same_side_it->second.recent_concurrent_samples) {
            if (sample.account_id == tracked.account_id) continue;
            sum += sample.concurrent_count;
            ++count;
        }
        if (count > 0) typical_concurrent = sum / static_cast<double>(count);
    }
    const double layering_score =
        config_.layering_saturation_count > 0
            ? std::clamp((static_cast<double>(concurrent) - typical_concurrent) /
                              static_cast<double>(config_.layering_saturation_count),
                          0.0, 1.0)
            : 0.0;

    const double primary = (depth_score + speed_score + move_score) / 3.0;
    const double combined = std::clamp(primary + config_.layering_bonus_weight * layering_score, 0.0, 1.0);

    if (combined < config_.alert_threshold) return {};

    Alert alert;
    alert.detector_name = name();
    alert.score = combined;
    alert.instrument_id = tracked.instrument_id;
    alert.account_ids = {tracked.account_id};
    alert.order_ids = {order.orig_order_id};
    alert.window_start_ns = tracked.placed_ts;
    alert.window_end_ns = order.timestamp_ns;
    alert.evidence = "depth_ratio=" + std::to_string(depth_score) + " speed=" + std::to_string(speed_score) +
                      " opposite_price_moved_favorably=" + (moved_favorably ? std::string("true") : "false") +
                      " move_score=" + std::to_string(move_score) +
                      " concurrent_same_side_orders=" + std::to_string(concurrent) +
                      " typical_concurrent=" + std::to_string(typical_concurrent) +
                      " layering_score=" + std::to_string(layering_score) +
                      " time_in_book_ns=" + std::to_string(time_in_book_ns);
    alert.book_snapshot_sequence = book.sequence();
    return {alert};
}

std::vector<Alert> SpoofingLayeringDetector::evaluate(const OrderBook& book, const DetectorEvent& incoming,
                                                       const AccountRegistry& /*accounts*/) {
    // Keeps ambient_by_instrument_side_ current for EVERY event, not just
    // ones this detector otherwise acts on -- any event can move a best
    // price, and move_score/layering_score both depend on that staying
    // up to date regardless of which branch below runs.
    if (const auto* order = std::get_if<Order>(&incoming)) {
        update_ambient(book, order->instrument_id, order->timestamp_ns);
    } else if (const auto* execution = std::get_if<Execution>(&incoming)) {
        update_ambient(book, execution->instrument_id, execution->timestamp_ns);
    }

    if (const auto* order = std::get_if<Order>(&incoming)) {
        switch (order->status) {
            case OrderStatus::kNew:
                track_new(book, *order);
                return {};
            case OrderStatus::kCancelled:
                return handle_cancel(book, *order);
            case OrderStatus::kReplaced:
                handle_replace(book, *order);
                return {};
        }
        return {};
    }
    if (const auto* execution = std::get_if<Execution>(&incoming)) {
        handle_execution(*execution);
    }
    return {};
}

}  // namespace tse::detectors
