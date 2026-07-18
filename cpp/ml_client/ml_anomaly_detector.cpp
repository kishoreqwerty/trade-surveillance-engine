#include "ml_anomaly_detector.hpp"

#include <variant>

namespace tse::ml_client {

using tse::fix::Order;
using tse::fix::OrderStatus;

MlAnomalyDetector::MlAnomalyDetector(MlScoringWorker* worker, MlAnomalyDetectorConfig config)
    : worker_(worker), config_(config) {}

void MlAnomalyDetector::handle_order(const Order& order) {
    const std::string key = order.account_id + "|" + order.instrument_id;
    WindowStats& stats = stats_by_key_[key];

    if (stats.window_start_ns == 0 || order.timestamp_ns - stats.window_start_ns > config_.window_duration_ns) {
        // Fresh tumbling window -- the previous window's stats are simply
        // superseded, not carried forward.
        stats = WindowStats{};
        stats.window_start_ns = order.timestamp_ns;
    }

    if (order.status == OrderStatus::kNew) {
        ++stats.order_count;
        stats.total_qty += order.qty;
    } else if (order.status == OrderStatus::kCancelled) {
        ++stats.cancel_count;
    }

    if (stats.order_count < config_.min_orders_before_submit) return;
    if (stats.order_count % config_.submit_every_n_orders != 0) return;

    const double window_duration_sec = static_cast<double>(config_.window_duration_ns) / 1e9;
    const double avg_qty =
        stats.order_count > 0 ? static_cast<double>(stats.total_qty) / static_cast<double>(stats.order_count) : 0.0;
    const double cancel_ratio =
        stats.order_count > 0 ? static_cast<double>(stats.cancel_count) / static_cast<double>(stats.order_count) : 0.0;

    ScoringRequest request;
    request.account_id = order.account_id;
    request.instrument_id = order.instrument_id;
    request.timestamp_ns = order.timestamp_ns;
    request.window_features = {
        {"order_count", static_cast<double>(stats.order_count)},
        {"total_qty", static_cast<double>(stats.total_qty)},
        {"avg_qty", avg_qty},
        {"cancel_ratio", cancel_ratio},
        {"orders_per_second", static_cast<double>(stats.order_count) / window_duration_sec},
    };

    worker_->submit(std::move(request));  // non-blocking -- see class comment
}

std::vector<tse::detectors::Alert> MlAnomalyDetector::evaluate(const tse::orderbook::OrderBook& /*book*/,
                                                                const tse::detectors::DetectorEvent& incoming,
                                                                const tse::detectors::AccountRegistry& /*accounts*/) {
    if (const auto* order = std::get_if<Order>(&incoming)) {
        handle_order(*order);
    }
    return {};  // always -- see class comment
}

}  // namespace tse::ml_client
