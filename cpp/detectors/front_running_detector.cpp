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
    // after" a much-later order it has nothing to do with. Left unpruned,
    // it would silently pair against later orders and produce a
    // nonsensical negative "Xns after" in the fired alert's evidence text.
    //
    // The bound is deliberately lookback_window_ns, not "any future
    // timestamp at all": a reacting order and the large order it follows
    // come from two *different* accounts -- plausibly different systems
    // with independently-clocked infrastructure, unlike
    // SpoofingLayeringDetector (same order_id, same sender, same clock,
    // for both halves of its own comparison). Ordinary cross-account clock
    // skew, even where it's enough to locally invert a same-side
    // comparison, should be small relative to the lookback window itself.
    // Bounding eviction to the window size only discards entries that are
    // stale/unrelated *garbage*, not ordinary skew.
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                 [&](const RecentOrder& r) {
                                     return std::llabs(order.timestamp_ns - r.timestamp_ns) > config_.lookback_window_ns;
                                 }),
                 recent.end());

    std::vector<Alert> alerts;
    // This order is the potential *reactor*: check whether it's small
    // relative to, and arrived shortly after, a related account's
    // already-recorded large order -- i.e. did this account trade with
    // advance knowledge that a large, not-yet-executed related-account
    // order was already pending? (See the class comment in
    // front_running_detector.hpp for why this direction -- reacting-order-
    // after-large-order, not before -- is the one that matches the
    // standard front-running definition, and matches what
    // cpp/simulator/abuse/front_running.cpp actually generates.)
    for (const RecentOrder& prior : recent) {
        if (prior.account_id == order.account_id) continue;
        if (!accounts.is_related(order.account_id, prior.account_id)) continue;
        if (prior.qty < config_.min_large_qty_threshold) continue;  // prior wasn't "large" enough to be worth front-running
        if (static_cast<double>(order.qty) > static_cast<double>(prior.qty) * config_.max_leader_to_large_size_ratio) {
            continue;
        }
        // This order must arrive at/after the large predecessor -- even
        // within the window bound above, e.g. a few milliseconds of
        // genuine cross-account clock skew -- cannot fire *for this
        // pairing* (the evidence text's "Xns after" would go negative).
        // Skipped, not erased from `recent`: unlike the prune step, this
        // must not permanently discard the large predecessor -- a later,
        // genuinely-ordered reacting order may still legitimately pair
        // with it once real elapsed time removes the ambiguity (see
        // FrontRunningDetectorTest.SlightlyFutureReactionIsNotPermanentlyMissed).
        if (order.timestamp_ns < prior.timestamp_ns) continue;

        Alert alert;
        alert.detector_name = name();
        alert.score = 1.0;
        alert.instrument_id = order.instrument_id;
        alert.account_ids = {prior.account_id, order.account_id};
        alert.order_ids = {prior.order_id, order.order_id};
        alert.window_start_ns = prior.timestamp_ns;
        alert.window_end_ns = order.timestamp_ns;
        alert.evidence = "related account " + order.account_id + " placed order " + order.order_id + " (qty=" +
                          std::to_string(order.qty) + ") " + std::to_string(order.timestamp_ns - prior.timestamp_ns) +
                          "ns after a large same-side order " + prior.order_id + " (qty=" +
                          std::to_string(prior.qty) + ") from related account " + prior.account_id;
        alert.book_snapshot_sequence = book_snapshot_sequence;
        alerts.push_back(std::move(alert));
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
