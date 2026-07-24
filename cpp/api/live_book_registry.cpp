#include "live_book_registry.hpp"

#include <algorithm>
#include <thread>
#include <variant>

namespace tse::api {

namespace {

BookEvent to_book_event(const tse::ingestion::IngestionEvent& event) {
    BookEvent out;
    if (std::holds_alternative<tse::fix::Order>(event)) {
        const tse::fix::Order& order = std::get<tse::fix::Order>(event);
        out.timestamp_ns = order.timestamp_ns;
        out.instrument_id = order.instrument_id;
        switch (order.status) {
            case tse::fix::OrderStatus::kCancelled:
                out.msg_type = "CANCEL";
                break;
            case tse::fix::OrderStatus::kReplaced:
                out.msg_type = "REPLACE";
                break;
            case tse::fix::OrderStatus::kNew:
            default:
                out.msg_type = "NEW";
                break;
        }
        out.side = order.side == tse::fix::Side::kBuy ? "BUY" : "SELL";
        out.price = order.price;
        out.qty = order.qty;
        out.order_id = order.order_id;
        out.account_id = order.account_id;
    } else {
        const tse::fix::Execution& execution = std::get<tse::fix::Execution>(event);
        out.timestamp_ns = execution.timestamp_ns;
        out.instrument_id = execution.instrument_id;
        out.msg_type = "EXECUTION";
        out.side = execution.side == tse::fix::Side::kBuy ? "BUY" : "SELL";
        out.price = execution.price;
        out.qty = execution.qty;
        out.order_id = execution.order_id;
        out.account_id = execution.account_id;
    }
    return out;
}

}  // namespace

void LiveBookRegistry::record_event(const tse::ingestion::IngestionEvent& event) {
    BookEvent recorded = to_book_event(event);
    std::deque<BookEvent>& events = recent_events_by_instrument_[recorded.instrument_id];
    events.push_back(std::move(recorded));
    while (events.size() > kMaxEventsPerInstrument) events.pop_front();
}

void LiveBookRegistry::process_one(const tse::ingestion::IngestionEvent& event) {
    tse::pipeline::ProcessResult result;
    {
        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        result = pipeline_.process(event);
        // Recorded regardless of skipped_inconsistent below -- the FIX
        // feed shows what arrived on the wire, not what the book did with
        // it (mirrors this file's class comment: a passive record of
        // externally-reported state, same stance order_book.hpp takes).
        record_event(event);
    }
    ++events_processed_;
    if (result.skipped_inconsistent) {
        ++events_skipped_inconsistent_;
        return;
    }
    if (alert_sink_ != nullptr) {
        for (const auto& alert : result.alerts) {
            alert_sink_->on_alert(alert);
        }
    }
}

void LiveBookRegistry::run(const std::atomic<bool>& producer_done) {
    tse::ingestion::IngestionEvent event;
    while (true) {
        if (queue_.pop(event)) {
            process_one(event);
            continue;
        }
        if (producer_done.load(std::memory_order_acquire)) {
            if (queue_.pop(event)) {
                process_one(event);
                continue;
            }
            break;
        }
        std::this_thread::yield();
    }
}

std::optional<tse::orderbook::DepthSnapshot> LiveBookRegistry::snapshot(const std::string& instrument_id) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    const tse::orderbook::OrderBook* book = pipeline_.book_for(instrument_id);
    if (book == nullptr) return std::nullopt;
    return book->snapshot();
}

std::vector<BookEvent> LiveBookRegistry::recent_events(const std::string& instrument_id, size_t limit) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    const auto it = recent_events_by_instrument_.find(instrument_id);
    if (it == recent_events_by_instrument_.end()) return {};
    const std::deque<BookEvent>& events = it->second;
    const size_t count = std::min(limit, events.size());
    return std::vector<BookEvent>(events.end() - static_cast<std::ptrdiff_t>(count), events.end());
}

}  // namespace tse::api
