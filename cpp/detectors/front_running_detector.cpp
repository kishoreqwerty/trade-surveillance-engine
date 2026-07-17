#include "front_running_detector.hpp"

#include <algorithm>

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

std::vector<Alert> FrontRunningDetector::handle_new(const Order& order, const AccountRegistry& accounts) {
    const std::string key = key_for(order.instrument_id, order.side);
    std::vector<RecentOrder>& recent = recent_by_key_[key];

    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                 [&](const RecentOrder& r) {
                                     return order.timestamp_ns - r.timestamp_ns > config_.lookback_window_ns;
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
            alerts.push_back(std::move(alert));
        }
    }

    recent.push_back(RecentOrder{order.account_id, order.order_id, order.qty, order.timestamp_ns});
    return alerts;
}

std::vector<Alert> FrontRunningDetector::evaluate(const tse::orderbook::OrderBook& /*book*/,
                                                   const DetectorEvent& incoming, const AccountRegistry& accounts) {
    const Order* order = std::get_if<Order>(&incoming);
    if (order == nullptr || order->status != OrderStatus::kNew) return {};
    return handle_new(*order, accounts);
}

}  // namespace tse::detectors
