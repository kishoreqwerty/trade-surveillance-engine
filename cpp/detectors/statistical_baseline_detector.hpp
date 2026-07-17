#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "i_detector.hpp"

namespace tse::detectors {

struct StatisticalBaselineConfig {
    double z_score_threshold{3.0};
    // Minimum prior observations for an account+instrument before a
    // z-score is considered meaningful -- with too few samples, the
    // running mean/stddev are themselves noise, and everything looks like
    // an outlier.
    int min_sample_count{5};
};

// "Z-score control, used later as the comparison point in the evaluation
// harness" (build guide, Phase 5) -- deliberately the simplest possible
// detector, on purpose: this exists to be the naive baseline Phase 10
// measures the pattern-aware detectors *against* ("how much better than a
// naive z-score is your spoofing detector"), so it should be genuinely
// naive, not itself a disguised heuristic.
//
// Tracks, per (account_id, instrument_id), a running mean and sample
// variance of New order quantity (Welford's algorithm -- numerically
// stable for a long-running streaming accumulator, unlike naively summing
// squares). On every New order, the z-score is computed against the
// running stats *as they stood before* this observation, then the
// observation is folded in -- scoring against pre-update stats is
// deliberate: including the new point in its own mean/variance first would
// damp its own deviation and make genuine outliers systematically harder
// to detect the more extreme they are.
//
// score = clamp(|z| / (2 * z_score_threshold), 0, 1) -- an order exactly
// at the threshold scores 0.5; one twice the threshold's z-distance scores
// 1.0. An arbitrary but simple, monotonic, documented mapping from an
// unbounded z-score to Alert's [0,1] convention.
//
// Reacts only to New Order events -- order size is the single feature
// tracked, by design (see above).
class StatisticalBaselineDetector : public IDetector {
public:
    explicit StatisticalBaselineDetector(StatisticalBaselineConfig config = {});

    std::vector<Alert> evaluate(const tse::orderbook::OrderBook& book, const DetectorEvent& incoming,
                                 const AccountRegistry& accounts) override;

    std::string name() const override { return "StatisticalBaselineDetector"; }

private:
    struct RunningStats {
        int64_t count{0};
        double mean{0.0};
        double m2{0.0};  // sum of squared differences from the running mean (Welford)
    };

    static void update(RunningStats& stats, double value);
    static double sample_variance(const RunningStats& stats);

    StatisticalBaselineConfig config_;
    std::unordered_map<std::string, RunningStats> stats_by_key_;  // key: account_id + "|" + instrument_id
};

}  // namespace tse::detectors
