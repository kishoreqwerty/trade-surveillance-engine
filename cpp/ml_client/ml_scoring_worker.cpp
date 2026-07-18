#include "ml_scoring_worker.hpp"

#include <thread>
#include <utility>

#include "alert.hpp"

namespace tse::ml_client {

MlScoringWorker::MlScoringWorker(MlScoreClient client, tse::pipeline::IAlertSink* alert_sink,
                                  MlScoringWorkerConfig config)
    : client_(std::move(client)), alert_sink_(alert_sink), config_(config), queue_(config.queue_capacity) {}

void MlScoringWorker::submit(ScoringRequest request) {
    queue_.push(std::move(request));  // drop-oldest under backpressure -- never blocks the caller
}

void MlScoringWorker::process_one(const ScoringRequest& request) {
    std::optional<ScoringResult> result = client_.score(request);
    if (!result.has_value()) {
        failed_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    scored_.fetch_add(1, std::memory_order_relaxed);
    if (result->anomaly_score < config_.alert_threshold) return;

    alerted_.fetch_add(1, std::memory_order_relaxed);
    tse::detectors::Alert alert;
    alert.detector_name = "MlAnomalyDetector";
    alert.score = result->anomaly_score;
    alert.instrument_id = request.instrument_id;
    alert.account_ids = {request.account_id};
    alert.window_start_ns = request.timestamp_ns;
    alert.window_end_ns = request.timestamp_ns;
    alert.evidence = "Isolation Forest anomaly_score=" + std::to_string(result->anomaly_score) +
                      " model_version=" + result->model_version;
    alert.model_version = result->model_version;
    if (alert_sink_ != nullptr) {
        alert_sink_->on_alert(alert);
    }
}

void MlScoringWorker::run(const std::atomic<bool>& stop_flag) {
    ScoringRequest request;
    while (true) {
        if (queue_.pop(request)) {
            process_one(request);
            continue;
        }
        if (stop_flag.load(std::memory_order_acquire)) {
            if (queue_.pop(request)) {
                process_one(request);
                continue;
            }
            break;
        }
        std::this_thread::yield();
    }
}

}  // namespace tse::ml_client
