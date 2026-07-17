#include "statistical_baseline_detector.hpp"

#include <algorithm>
#include <cmath>

namespace tse::detectors {

using tse::fix::Order;
using tse::fix::OrderStatus;

StatisticalBaselineDetector::StatisticalBaselineDetector(StatisticalBaselineConfig config) : config_(config) {}

void StatisticalBaselineDetector::update(RunningStats& stats, double value) {
    stats.count += 1;
    const double delta = value - stats.mean;
    stats.mean += delta / static_cast<double>(stats.count);
    const double delta2 = value - stats.mean;
    stats.m2 += delta * delta2;
}

double StatisticalBaselineDetector::sample_variance(const RunningStats& stats) {
    return stats.count > 1 ? stats.m2 / static_cast<double>(stats.count - 1) : 0.0;
}

std::vector<Alert> StatisticalBaselineDetector::evaluate(const tse::orderbook::OrderBook& /*book*/,
                                                          const DetectorEvent& incoming,
                                                          const AccountRegistry& /*accounts*/) {
    const Order* order = std::get_if<Order>(&incoming);
    if (order == nullptr || order->status != OrderStatus::kNew) return {};

    const std::string key = order->account_id + "|" + order->instrument_id;
    RunningStats& stats = stats_by_key_[key];

    std::vector<Alert> alerts;
    if (stats.count >= config_.min_sample_count) {
        const double stddev = std::sqrt(sample_variance(stats));
        if (stddev > 0.0) {
            const double z = (static_cast<double>(order->qty) - stats.mean) / stddev;
            if (std::abs(z) >= config_.z_score_threshold) {
                Alert alert;
                alert.detector_name = name();
                alert.score = std::clamp(std::abs(z) / (2.0 * config_.z_score_threshold), 0.0, 1.0);
                alert.instrument_id = order->instrument_id;
                alert.account_ids = {order->account_id};
                alert.order_ids = {order->order_id};
                alert.window_start_ns = order->timestamp_ns;
                alert.window_end_ns = order->timestamp_ns;
                alert.evidence = "order qty=" + std::to_string(order->qty) + " z=" + std::to_string(z) +
                                  " against running mean=" + std::to_string(stats.mean) +
                                  " stddev=" + std::to_string(stddev) + " (n=" + std::to_string(stats.count) + ")";
                alerts.push_back(std::move(alert));
            }
        }
    }

    update(stats, static_cast<double>(order->qty));
    return alerts;
}

}  // namespace tse::detectors
