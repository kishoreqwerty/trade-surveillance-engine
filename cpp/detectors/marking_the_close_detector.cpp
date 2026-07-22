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

void MarkingTheCloseDetector::register_and_group(const std::string& account_id, const AccountRegistry& accounts) {
    if (all_accounts_seen_.count(account_id) != 0) return;  // already registered, already grouped
    group_parent_[account_id] = account_id;                 // starts as its own singleton
    for (const auto& other : all_accounts_seen_) {
        if (accounts.is_related(account_id, other)) {
            const std::string root_a = find_group(account_id);
            const std::string root_b = find_group(other);
            if (root_a != root_b) group_parent_[root_a] = root_b;
        }
    }
    all_accounts_seen_.insert(account_id);
}

std::string MarkingTheCloseDetector::find_group(const std::string& account_id) {
    auto it = group_parent_.find(account_id);
    if (it == group_parent_.end() || it->second == account_id) return account_id;
    const std::string root = find_group(it->second);
    it->second = root;  // path compression
    return root;
}

std::vector<std::string> MarkingTheCloseDetector::group_members(const std::string& account_id) {
    const std::string rep = find_group(account_id);
    std::vector<std::string> members;
    for (const auto& acc : all_accounts_seen_) {
        if (find_group(acc) == rep) members.push_back(acc);
    }
    return members;
}

std::optional<Alert> MarkingTheCloseDetector::check_account(const std::string& instrument_id,
                                                             const std::string& account_id, int64_t window_start_ns,
                                                             int64_t event_ts, int64_t book_snapshot_sequence,
                                                             const AccountRegistry& accounts) {
    register_and_group(account_id, accounts);
    const std::vector<std::string> members = group_members(account_id);

    // Group-level dedup: if ANY current member already triggered (even
    // under a since-superseded group composition -- see find_group()'s own
    // comment on why members are re-resolved fresh, not cached), the whole
    // group is considered already alerted. Discovering a new related
    // account later must not cause a spurious re-fire for a group that's
    // already been flagged.
    for (const auto& member : members) {
        if (alerted_.count(key_for(instrument_id, member)) != 0) return std::nullopt;
    }

    int64_t group_qty = 0;
    std::vector<std::string> group_trade_ids;
    for (const auto& member : members) {
        const std::string member_key = key_for(instrument_id, member);
        group_qty += account_window_qty_[member_key];
        auto ids_it = trade_ids_by_key_.find(member_key);
        if (ids_it != trade_ids_by_key_.end()) {
            group_trade_ids.insert(group_trade_ids.end(), ids_it->second.begin(), ids_it->second.end());
        }
    }
    if (group_qty < config_.min_account_qty_threshold) return std::nullopt;

    const int64_t total_qty = total_window_qty_[instrument_id];
    if (total_qty < config_.min_total_window_qty_threshold) return std::nullopt;

    const double share = static_cast<double>(group_qty) / static_cast<double>(total_qty);
    if (share < config_.concentration_threshold) return std::nullopt;

    alerted_.insert(key_for(instrument_id, account_id));

    Alert alert;
    alert.detector_name = name();
    alert.score = std::min(share, 1.0);
    alert.instrument_id = instrument_id;
    alert.account_ids = members;
    alert.order_ids = group_trade_ids;
    alert.window_start_ns = window_start_ns;
    alert.window_end_ns = event_ts;
    alert.evidence = "account-group closing-window volume=" + std::to_string(group_qty) + " / total=" +
                      std::to_string(total_qty) + " (share=" + std::to_string(share) +
                      ", group_size=" + std::to_string(members.size()) + ")";
    alert.book_snapshot_sequence = book_snapshot_sequence;
    return alert;
}

std::vector<Alert> MarkingTheCloseDetector::handle_execution(const Execution& execution,
                                                               int64_t book_snapshot_sequence,
                                                               const AccountRegistry& accounts) {
    auto close_it = config_.close_time_ns_by_instrument.find(execution.instrument_id);
    if (close_it == config_.close_time_ns_by_instrument.end()) return {};  // no known session boundary
    const int64_t close_ts = close_it->second;
    const int64_t window_start_ns = close_ts - config_.window_duration_ns;
    if (execution.timestamp_ns < window_start_ns || execution.timestamp_ns > close_ts) return {};

    total_window_qty_[execution.instrument_id] += execution.qty;
    account_window_qty_[key_for(execution.instrument_id, execution.account_id)] += execution.qty;
    trade_ids_by_key_[key_for(execution.instrument_id, execution.account_id)].push_back(execution.trade_id);
    if (!execution.counterparty_account_id.empty() &&
        execution.counterparty_account_id != execution.account_id) {
        account_window_qty_[key_for(execution.instrument_id, execution.counterparty_account_id)] += execution.qty;
        trade_ids_by_key_[key_for(execution.instrument_id, execution.counterparty_account_id)].push_back(
            execution.trade_id);
    }

    std::vector<Alert> alerts;
    if (auto alert = check_account(execution.instrument_id, execution.account_id, window_start_ns,
                                    execution.timestamp_ns, book_snapshot_sequence, accounts)) {
        alerts.push_back(std::move(*alert));
    }
    if (!execution.counterparty_account_id.empty() &&
        execution.counterparty_account_id != execution.account_id) {
        if (auto alert = check_account(execution.instrument_id, execution.counterparty_account_id, window_start_ns,
                                        execution.timestamp_ns, book_snapshot_sequence, accounts)) {
            alerts.push_back(std::move(*alert));
        }
    }
    return alerts;
}

std::vector<Alert> MarkingTheCloseDetector::evaluate(const tse::orderbook::OrderBook& book,
                                                      const DetectorEvent& incoming,
                                                      const AccountRegistry& accounts) {
    const Execution* execution = std::get_if<Execution>(&incoming);
    if (execution == nullptr) return {};
    return handle_execution(*execution, book.sequence(), accounts);
}

}  // namespace tse::detectors
