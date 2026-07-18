#include "live_book_registry.hpp"

#include <thread>

namespace tse::api {

void LiveBookRegistry::process_one(const tse::ingestion::IngestionEvent& event) {
    tse::pipeline::ProcessResult result;
    {
        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        result = pipeline_.process(event);
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

}  // namespace tse::api
