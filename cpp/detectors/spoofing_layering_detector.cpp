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
}  // namespace

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
}

void SpoofingLayeringDetector::handle_replace(const OrderBook& book, const Order& order) {
    tracked_.erase(order.orig_order_id);  // old identity retired without emitting anything (see class comment)
    track_new(book, order);               // new identity starts a fresh lifecycle
}

void SpoofingLayeringDetector::handle_execution(const Execution& execution) {
    auto it = tracked_.find(execution.order_id);
    if (it == tracked_.end()) return;
    it->second.remaining_qty -= execution.qty;
    if (it->second.remaining_qty <= 0) {
        tracked_.erase(it);  // fully executed: genuine flow, not spoofing -- just stop tracking it
    }
}

int SpoofingLayeringDetector::count_concurrent_same_account_same_side(const std::string& account_id,
                                                                       const std::string& instrument_id,
                                                                       Side side) const {
    int count = 0;
    for (const auto& [order_id, tracked] : tracked_) {
        if (tracked.account_id == account_id && tracked.instrument_id == instrument_id && tracked.side == side) {
            ++count;
        }
    }
    return count;
}

std::vector<Alert> SpoofingLayeringDetector::handle_cancel(const OrderBook& book, const Order& order) {
    auto it = tracked_.find(order.orig_order_id);
    if (it == tracked_.end()) return {};  // not an order this detector was tracking -- silently ignore
    const TrackedOrder tracked = it->second;
    tracked_.erase(it);  // lifecycle complete either way

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
    const double move_score = moved_favorably ? 1.0 : 0.0;

    const int concurrent = count_concurrent_same_account_same_side(tracked.account_id, tracked.instrument_id,
                                                                     tracked.side);
    const double layering_score = config_.layering_saturation_count > 0
                                       ? std::clamp(static_cast<double>(concurrent) /
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
                      " concurrent_same_side_orders=" + std::to_string(concurrent) +
                      " time_in_book_ns=" + std::to_string(time_in_book_ns);
    return {alert};
}

std::vector<Alert> SpoofingLayeringDetector::evaluate(const OrderBook& book, const DetectorEvent& incoming,
                                                       const AccountRegistry& /*accounts*/) {
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
