#include "wash_trade_detector.hpp"

namespace tse::detectors {

using tse::fix::Execution;

std::vector<Alert> WashTradeDetector::evaluate(const tse::orderbook::OrderBook& book, const DetectorEvent& incoming,
                                                const AccountRegistry& accounts) {
    const Execution* execution = std::get_if<Execution>(&incoming);
    if (execution == nullptr) return {};
    if (execution->counterparty_account_id.empty()) return {};  // nothing to compare against
    if (!accounts.is_related(execution->account_id, execution->counterparty_account_id)) return {};

    Alert alert;
    alert.detector_name = name();
    alert.score = 1.0;
    alert.instrument_id = execution->instrument_id;
    alert.account_ids = {execution->account_id, execution->counterparty_account_id};
    alert.order_ids = {execution->order_id, execution->trade_id};
    alert.window_start_ns = execution->timestamp_ns;
    alert.window_end_ns = execution->timestamp_ns;
    alert.evidence = "Execution " + execution->trade_id + " matched account " + execution->account_id +
                      " against related counterparty " + execution->counterparty_account_id +
                      " (same beneficial owner, explicit link, or self-trade)";
    alert.book_snapshot_sequence = book.sequence();
    return {alert};
}

}  // namespace tse::detectors
