#include "front_running_detector.hpp"

#include <algorithm>
#include <cstdlib>

namespace tse::detectors {

using tse::fix::Order;
using tse::fix::OrderStatus;
using tse::fix::Side;

FrontRunningDetector::FrontRunningDetector(FrontRunningConfig config) : config_(config) {}

namespace {
std::string key_for(const std::string& instrument_id, Side side) {
    return instrument_id + "|" + (side == Side::kBuy ? "B" : "S");
}
}  // namespace

std::vector<Alert> FrontRunningDetector::handle_new(const Order& order, const AccountRegistry& accounts,
                                                     int64_t book_snapshot_sequence) {
    const std::string key = key_for(order.instrument_id, order.side);
    std::vector<RecentOrder>& recent = recent_by_key_[key];

    // Prunes entries more than lookback_window_ns away from the current
    // order's timestamp in *either* direction. The forward case (too old)
    // is the original, normal pruning. The backward case (timestamped
    // implausibly far in the *future* relative to the event being
    // processed right now) exists because of a real bug: cpp/ingestion/'s
    // drop-oldest backpressure policy can drop a New outright (see
    // cpp/ingestion/README.md), and a multi-session demo/replay feed can
    // legitimately restart its clock (cpp/api/main.cpp loops a fresh
    // synthetic session, each with its own small relative timestamps, into
    // one long-lived pipeline) -- either way, a leftover entry from an
    // entirely different, unrelated lifecycle can end up "timestamped
    // after" a much-later order it has nothing to do with. Left unpruned
    // (the original bug: a negative diff was never `> lookback_window_ns`,
    // so such an entry lingered forever), it silently paired against later
    // orders and produced a nonsensical negative "Xns ahead of" in the
    // fired alert's evidence text.
    //
    // The bound is deliberately lookback_window_ns, not "any future
    // timestamp at all": a leader and the large order it precedes come
    // from two *different* accounts -- plausibly different systems with
    // independently-clocked infrastructure, unlike SpoofingLayeringDetector
    // (same order_id, same sender, same clock, for both halves of its own
    // comparison). Ordinary cross-account clock skew, even where it's
    // enough to locally invert a same-side comparison, should be small
    // relative to the lookback window itself -- a genuine leader is by
    // definition within that window of the order it's leading. Pruning
    // strictly on "any future timestamp" would have risked discarding a
    // real leader over a few microseconds of skew and never getting
    // another chance at it; bounding eviction to the window size only
    // discards entries that are stale/unrelated *garbage* relative to
    // what this order could plausibly be leading, exactly the demo-loop
    // and drop-induced cases above (which showed tens-of-*seconds*-scale
    // violations, not microseconds).
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                 [&](const RecentOrder& r) {
                                     return std::llabs(order.timestamp_ns - r.timestamp_ns) > config_.lookback_window_ns;
                                 }),
                 recent.end());

    std::vector<Alert> alerts;
    if (order.qty >= config_.min_large_qty_threshold) {
        for (const RecentOrder& leader : recent) {
            if (leader.account_id == order.account_id) continue;
            if (!accounts.is_related(order.account_id, leader.account_id)) continue;
            if (static_cast<double>(leader.qty) > static_cast<double>(order.qty) * config_.max_leader_to_large_size_ratio) {
                continue;
            }
            // A leader that's still timestamped after this order -- even
            // if within the window bound above, e.g. a few milliseconds
            // of genuine cross-account clock skew -- cannot fire *for
            // this pairing* (the evidence text's "Xns ahead of" would go
            // negative). Skipped, not erased from `recent`: unlike the
            // prune step, this must not permanently discard the entry --
            // a later, larger order whose own timestamp genuinely clears
            // this one may still legitimately pair with it once real
            // elapsed time removes the ambiguity (see
            // FrontRunningDetectorTest.SlightlyFutureLeaderIsNotPermanentlyDiscarded).
            if (leader.timestamp_ns > order.timestamp_ns) continue;

            Alert alert;
            alert.detector_name = name();
            alert.score = 1.0;
            alert.instrument_id = order.instrument_id;
            alert.account_ids = {leader.account_id, order.account_id};
            alert.order_ids = {leader.order_id, order.order_id};
            alert.window_start_ns = leader.timestamp_ns;
            alert.window_end_ns = order.timestamp_ns;
            alert.evidence = "related account " + leader.account_id + " placed order " + leader.order_id +
                              " (qty=" + std::to_string(leader.qty) + ") " +
                              std::to_string(order.timestamp_ns - leader.timestamp_ns) +
                              "ns ahead of a large same-side order " + order.order_id + " (qty=" +
                              std::to_string(order.qty) + ") from account " + order.account_id;
            alert.book_snapshot_sequence = book_snapshot_sequence;
            alerts.push_back(std::move(alert));
        }
    }

    recent.push_back(RecentOrder{order.account_id, order.order_id, order.qty, order.timestamp_ns});
    return alerts;
}

std::vector<Alert> FrontRunningDetector::evaluate(const tse::orderbook::OrderBook& book,
                                                   const DetectorEvent& incoming, const AccountRegistry& accounts) {
    const Order* order = std::get_if<Order>(&incoming);
    if (order == nullptr || order->status != OrderStatus::kNew) return {};
    return handle_new(*order, accounts, book.sequence());
}

}  // namespace tse::detectors
