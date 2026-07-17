#include "live_consumer.hpp"

#include <thread>

namespace tse::pipeline {

void LiveConsumer::process_one(const tse::ingestion::IngestionEvent& event) {
    // IngestionEvent and DetectorEvent are the same underlying
    // std::variant<Order, Execution> type -- two independent aliases in
    // different namespaces, deliberately (see detector_event.hpp) -- so
    // this passes through with no conversion.
    ProcessResult result = pipeline_.process(event);
    ++events_processed_;
    if (result.skipped_inconsistent) {
        ++events_skipped_inconsistent_;
        return;
    }
    latency_samples_.push_back(EventLatencySample{result.book_apply_ns, result.detectors_ns});
    if (alert_sink_ != nullptr) {
        for (const auto& alert : result.alerts) {
            alert_sink_->on_alert(alert);
        }
    }
}

void LiveConsumer::run(const std::atomic<bool>& producer_done) {
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

}  // namespace tse::pipeline
