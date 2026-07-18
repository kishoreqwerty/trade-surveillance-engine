#include "marking_the_close_detector.hpp"

#include <algorithm>

namespace tse::detectors {

using tse::fix::Execution;

MarkingTheCloseDetector::MarkingTheCloseDetector(MarkingTheCloseConfig config) : config_(std::move(config)) {}

namespace {
std::string key_for(const std::string& instrument_id, const std::string& account_id) {
    return instrument_id + "|" + account_id;
}
}  // namespace

std::optional<Alert> MarkingTheCloseDetector::check_account(const std::string& instrument_id,
                                                             const std::string& account_id, int64_t window_start_ns,
                                                             int64_t event_ts, int64_t book_snapshot_sequence) {
    const std::string key = key_for(instrument_id, account_id);
    if (alerted_.count(key) != 0) return std::nullopt;  // already fired for this account this window

    const int64_t account_qty = account_window_qty_[key];
    if (account_qty < config_.min_account_qty_threshold) return std::nullopt;

    const int64_t total_qty = total_window_qty_[instrument_id];
    if (total_qty < config_.min_total_window_qty_threshold) return std::nullopt;

    const double share = static_cast<double>(account_qty) / static_cast<double>(total_qty);
    if (share < config_.concentration_threshold) return std::nullopt;

    alerted_.insert(key);

    Alert alert;
    alert.detector_name = name();
    alert.score = std::min(share, 1.0);
    alert.instrument_id = instrument_id;
    alert.account_ids = {account_id};
    alert.window_start_ns = window_start_ns;
    alert.window_end_ns = event_ts;
    alert.evidence = "account closing-window volume=" + std::to_string(account_qty) + " / total=" +
                      std::to_string(total_qty) + " (share=" + std::to_string(share) + ")";
    alert.book_snapshot_sequence = book_snapshot_sequence;
    return alert;
}

std::vector<Alert> MarkingTheCloseDetector::handle_execution(const Execution& execution,
                                                               int64_t book_snapshot_sequence) {
    auto close_it = config_.close_time_ns_by_instrument.find(execution.instrument_id);
    if (close_it == config_.close_time_ns_by_instrument.end()) return {};  // no known session boundary
    const int64_t close_ts = close_it->second;
    const int64_t window_start_ns = close_ts - config_.window_duration_ns;
    if (execution.timestamp_ns < window_start_ns || execution.timestamp_ns > close_ts) return {};

    total_window_qty_[execution.instrument_id] += execution.qty;
    account_window_qty_[key_for(execution.instrument_id, execution.account_id)] += execution.qty;
    if (!execution.counterparty_account_id.empty() &&
        execution.counterparty_account_id != execution.account_id) {
        account_window_qty_[key_for(execution.instrument_id, execution.counterparty_account_id)] += execution.qty;
    }

    std::vector<Alert> alerts;
    if (auto alert = check_account(execution.instrument_id, execution.account_id, window_start_ns,
                                    execution.timestamp_ns, book_snapshot_sequence)) {
        alerts.push_back(std::move(*alert));
    }
    if (!execution.counterparty_account_id.empty() &&
        execution.counterparty_account_id != execution.account_id) {
        if (auto alert = check_account(execution.instrument_id, execution.counterparty_account_id, window_start_ns,
                                        execution.timestamp_ns, book_snapshot_sequence)) {
            alerts.push_back(std::move(*alert));
        }
    }
    return alerts;
}

std::vector<Alert> MarkingTheCloseDetector::evaluate(const tse::orderbook::OrderBook& book,
                                                      const DetectorEvent& incoming,
                                                      const AccountRegistry& /*accounts*/) {
    const Execution* execution = std::get_if<Execution>(&incoming);
    if (execution == nullptr) return {};
    return handle_execution(*execution, book.sequence());
}

}  // namespace tse::detectors
