#include "evaluation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace tse::harness {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}

double ConfusionMatrix::precision() const {
    return (tp + fp) > 0 ? static_cast<double>(tp) / static_cast<double>(tp + fp) : kNaN;
}

double ConfusionMatrix::recall() const {
    return (tp + fn) > 0 ? static_cast<double>(tp) / static_cast<double>(tp + fn) : kNaN;
}

double ConfusionMatrix::f1() const {
    const double p = precision();
    const double r = recall();
    if (std::isnan(p) || std::isnan(r) || (p + r) == 0.0) return kNaN;
    return 2.0 * p * r / (p + r);
}

Universes build_universes(const tse::simulator::SimulationOutput& simulation) {
    Universes universes;
    for (const auto& order : simulation.orders) {
        DetectorUniverse* target = nullptr;
        if (order.status == tse::simulator::OrderStatus::kNew) {
            target = &universes.new_orders;
        } else if (order.status == tse::simulator::OrderStatus::kCancelled) {
            target = &universes.cancel_orders;
        } else {
            continue;  // kReplaced: not currently emitted by any generator, and no detector's evidence keys off it
        }
        target->event_ids.insert(order.order_id);
        target->ids_by_pattern[order.ground_truth_label.pattern].insert(order.order_id);
    }
    for (const auto& execution : simulation.executions) {
        universes.executions.event_ids.insert(execution.trade_id);
        universes.executions.ids_by_pattern[execution.ground_truth_label.pattern].insert(execution.trade_id);
    }
    return universes;
}

const std::unordered_set<std::string>& positive_ids_for(const DetectorUniverse& universe,
                                                          tse::simulator::AbusePattern target_pattern) {
    static const std::unordered_set<std::string> empty_set;
    auto it = universe.ids_by_pattern.find(target_pattern);
    return it == universe.ids_by_pattern.end() ? empty_set : it->second;
}

std::unordered_set<std::string> any_abuse_ids(const DetectorUniverse& universe) {
    std::unordered_set<std::string> result;
    for (const auto& [pattern, ids] : universe.ids_by_pattern) {
        if (pattern == tse::simulator::AbusePattern::kBaseline) continue;
        result.insert(ids.begin(), ids.end());
    }
    return result;
}

ConfusionMatrix compute_confusion_matrix(const std::vector<tse::detectors::Alert>& alerts,
                                          const std::string& detector_name, double score_threshold,
                                          const std::unordered_set<std::string>& universe_ids,
                                          const std::unordered_set<std::string>& positive_ids) {
    std::unordered_set<std::string> predicted;
    for (const auto& alert : alerts) {
        if (alert.detector_name != detector_name) continue;
        if (alert.score < score_threshold) continue;
        for (const auto& id : alert.order_ids) {
            if (universe_ids.count(id) != 0) predicted.insert(id);
        }
    }

    ConfusionMatrix matrix;
    for (const auto& id : predicted) {
        if (positive_ids.count(id) != 0) {
            ++matrix.tp;
        } else {
            ++matrix.fp;
        }
    }
    for (const auto& id : positive_ids) {
        if (predicted.count(id) == 0) ++matrix.fn;
    }
    for (const auto& id : universe_ids) {
        if (positive_ids.count(id) == 0 && predicted.count(id) == 0) ++matrix.tn;
    }
    return matrix;
}

std::vector<SweepPoint> threshold_sweep(const std::vector<tse::detectors::Alert>& alerts,
                                         const std::string& detector_name,
                                         const std::unordered_set<std::string>& universe_ids,
                                         const std::unordered_set<std::string>& positive_ids,
                                         const std::vector<double>& thresholds) {
    std::vector<SweepPoint> sweep;
    sweep.reserve(thresholds.size());
    for (double threshold : thresholds) {
        sweep.push_back({threshold, compute_confusion_matrix(alerts, detector_name, threshold, universe_ids, positive_ids)});
    }
    return sweep;
}

const std::vector<DetectorSpec>& pattern_aware_detector_specs() {
    static const std::vector<DetectorSpec> specs = {
        {"WashTradeDetector", tse::simulator::AbusePattern::kWashTrade},
        {"SpoofingLayeringDetector", tse::simulator::AbusePattern::kSpoofingLayering},
        {"MarkingTheCloseDetector", tse::simulator::AbusePattern::kMarkingTheClose},
        {"FrontRunningDetector", tse::simulator::AbusePattern::kFrontRunning},
    };
    return specs;
}

UniverseKind detector_universe_kind(const std::string& detector_name) {
    if (detector_name == "WashTradeDetector") return UniverseKind::kExecution;
    if (detector_name == "SpoofingLayeringDetector") return UniverseKind::kCancelOrder;
    if (detector_name == "MarkingTheCloseDetector") return UniverseKind::kExecution;
    if (detector_name == "FrontRunningDetector") return UniverseKind::kNewOrder;
    if (detector_name == "StatisticalBaselineDetector") return UniverseKind::kNewOrder;
    throw std::invalid_argument("detector_universe_kind: unknown detector '" + detector_name + "'");
}

const DetectorUniverse& select_universe(const Universes& universes, UniverseKind kind) {
    switch (kind) {
        case UniverseKind::kNewOrder:
            return universes.new_orders;
        case UniverseKind::kCancelOrder:
            return universes.cancel_orders;
        case UniverseKind::kExecution:
            return universes.executions;
    }
    throw std::invalid_argument("select_universe: unhandled UniverseKind");
}

MlAnomalyGroundTruth build_ml_anomaly_ground_truth(const tse::simulator::SimulationOutput& simulation) {
    MlAnomalyGroundTruth result;
    for (const auto& order : simulation.orders) {
        if (order.ground_truth_label.pattern == tse::simulator::AbusePattern::kBaseline) continue;
        const std::string key = order.account_id + "|" + order.instrument_id;
        result.abuse_order_timestamps_by_key[key].push_back(order.timestamp_ns);
    }
    for (auto& [key, timestamps] : result.abuse_order_timestamps_by_key) {
        std::sort(timestamps.begin(), timestamps.end());
    }
    return result;
}

bool ml_anomaly_window_is_positive(const MlAnomalyGroundTruth& ground_truth, const tse::detectors::Alert& alert,
                                    int64_t window_duration_ns) {
    if (alert.account_ids.empty()) return false;
    const std::string key = alert.account_ids.front() + "|" + alert.instrument_id;
    const auto it = ground_truth.abuse_order_timestamps_by_key.find(key);
    if (it == ground_truth.abuse_order_timestamps_by_key.end()) return false;

    const int64_t window_hi = alert.window_start_ns;  // == window_end_ns, see header comment
    const int64_t window_lo = window_hi - window_duration_ns;
    const std::vector<int64_t>& timestamps = it->second;
    const auto first_in_range = std::lower_bound(timestamps.begin(), timestamps.end(), window_lo);
    return first_in_range != timestamps.end() && *first_in_range <= window_hi;
}

ConfusionMatrix compute_ml_anomaly_confusion_matrix(const std::vector<tse::detectors::Alert>& alerts,
                                                      double score_threshold, const MlAnomalyGroundTruth& ground_truth,
                                                      int64_t window_duration_ns) {
    ConfusionMatrix matrix;
    for (const auto& alert : alerts) {
        if (alert.detector_name != "MlAnomalyDetector") continue;
        const bool positive = ml_anomaly_window_is_positive(ground_truth, alert, window_duration_ns);
        const bool predicted = alert.score >= score_threshold;
        if (predicted && positive) {
            ++matrix.tp;
        } else if (predicted && !positive) {
            ++matrix.fp;
        } else if (!predicted && positive) {
            ++matrix.fn;
        } else {
            ++matrix.tn;
        }
    }
    return matrix;
}

std::vector<SweepPoint> ml_anomaly_threshold_sweep(const std::vector<tse::detectors::Alert>& alerts,
                                                     const MlAnomalyGroundTruth& ground_truth,
                                                     int64_t window_duration_ns, const std::vector<double>& thresholds) {
    std::vector<SweepPoint> sweep;
    sweep.reserve(thresholds.size());
    for (double threshold : thresholds) {
        sweep.push_back({threshold, compute_ml_anomaly_confusion_matrix(alerts, threshold, ground_truth, window_duration_ns)});
    }
    return sweep;
}

}  // namespace tse::harness
