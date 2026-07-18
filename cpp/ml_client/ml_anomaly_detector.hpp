#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "i_detector.hpp"
#include "ml_scoring_worker.hpp"

namespace tse::ml_client {

struct MlAnomalyDetectorConfig {
    // Matches ml_service's own fixed WINDOW_DURATION_SECONDS assumption
    // (app/training_data.py) -- a plain tumbling window, not carried
    // forward across boundaries.
    int64_t window_duration_ns{60'000'000'000LL};  // 60s
    int min_orders_before_submit{5};                // avoid scoring near-empty windows
    int submit_every_n_orders{5};                   // periodic, not on every single order
};

// The IDetector-conforming face of the ML scoring path. Registering this
// alongside the five synchronous detectors lets LivePipeline (and, later,
// Phase 10's harness) treat it uniformly through the same interface — but
// evaluate() *never* blocks: all it does synchronously is maintain a
// small per-(account, instrument) running tally of volume/frequency
// features and, periodically, hand a non-blocking submit() to
// MlScoringWorker. evaluate() unconditionally returns an empty vector —
// any Alert this pattern produces arrives later, asynchronously, directly
// from the worker thread to the shared IAlertSink, never through this
// method's return value. This is what makes "async, off the hot path"
// (architecture doc §3) a structural property of this class rather than
// a promise about how it's used — there is no code path inside evaluate()
// that can block on the network.
class MlAnomalyDetector : public tse::detectors::IDetector {
public:
    MlAnomalyDetector(MlScoringWorker* worker, MlAnomalyDetectorConfig config = {});

    std::vector<tse::detectors::Alert> evaluate(const tse::orderbook::OrderBook& book,
                                                 const tse::detectors::DetectorEvent& incoming,
                                                 const tse::detectors::AccountRegistry& accounts) override;

    std::string name() const override { return "MlAnomalyDetector"; }

private:
    struct WindowStats {
        int64_t window_start_ns{0};
        int64_t order_count{0};
        int64_t total_qty{0};
        int64_t cancel_count{0};
    };

    void handle_order(const tse::fix::Order& order);

    MlScoringWorker* worker_;
    MlAnomalyDetectorConfig config_;
    std::unordered_map<std::string, WindowStats> stats_by_key_;
};

}  // namespace tse::ml_client
