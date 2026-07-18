// Phase 10's "Done when" entrypoint: real, reproducible precision/recall/F1
// numbers per detector, a threshold sweep + confusion matrix per detector,
// a difficulty-gradient curve against Phase 1's severity parameter, and an
// explicit comparison between each pattern-aware detector and
// StatisticalBaselineDetector -- all computed from Alerts produced by one
// real replay of Phase 1 output through Kafka into pipeline/'s actual
// LivePipeline (see replay_runner.cpp), never a separate offline scorer.
//
// Every number below is printed as measured, at every threshold in the
// sweep -- per CLAUDE.md's "report the full sweep honestly ... do not
// cherry-pick the best threshold," this program does not pick a "winning"
// threshold anywhere.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "evaluation.hpp"
#include "ground_truth.hpp"
#include "replay_runner.hpp"
#include "simulator.hpp"

using tse::simulator::AbusePattern;
using tse::simulator::SimulationOutput;
using tse::simulator::SimulatorConfig;

namespace {

constexpr const char* kBrokers = "localhost:9092";
// Fixed, non-wall-clock anchor -- an evaluation harness cares about
// reproducibility across runs, not "looks live right now" (contrast
// cpp/api/main.cpp's demo server, which deliberately uses a wall-clock
// anchor for exactly the opposite reason).
constexpr int64_t kEpochAnchorNs = 1'700'000'000'000'000'000LL;

std::string unique_topic(const std::string& label) {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "tse-harness-eval-" + label + "-" + std::to_string(now);
}

std::string fmt(double value) {
    if (std::isnan(value)) return "  n/a";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.3f", value);
    return buf;
}

void print_matrix_row(const std::string& label, const tse::harness::ConfusionMatrix& m) {
    std::printf("  %-42s TP=%-5llu FP=%-5llu FN=%-5llu TN=%-6llu  P=%-8s R=%-8s F1=%-8s\n", label.c_str(),
                static_cast<unsigned long long>(m.tp), static_cast<unsigned long long>(m.fp),
                static_cast<unsigned long long>(m.fn), static_cast<unsigned long long>(m.tn), fmt(m.precision()).c_str(),
                fmt(m.recall()).c_str(), fmt(m.f1()).c_str());
}

SimulatorConfig main_eval_config() {
    SimulatorConfig config;
    config.random_seed = 777;
    config.session_start_ns = kEpochAnchorNs;
    config.session_duration_ns = 600LL * 1'000'000'000;  // 10 synthetic minutes
    config.baseline_orders_per_second = 3.0;
    config.num_equity_instruments = 8;
    config.num_fx_instruments = 4;
    config.num_fixed_income_instruments = 3;
    config.num_independent_accounts = 150;
    config.num_linked_account_pairs = 30;
    config.wash_trade = {15, 0.5};
    config.spoofing_layering = {15, 0.5};
    config.marking_the_close = {15, 0.5};
    config.front_running = {15, 0.5};
    return config;
}

// StatisticalBaselineDetector needs config_.min_sample_count (default 5)
// PRIOR New orders on the exact same (account_id, instrument_id) key
// before it can compute a z-score at all -- see
// statistical_baseline_detector.cpp. main_eval_config()'s realistic 150
// accounts x 15 instruments spread over a 10-minute session means the vast
// majority of keys never accumulate that history (measured: ~1.7% of keys
// reach it), so the naive baseline barely gets to *try*, let alone catch
// anything -- a real, honestly-reported limitation of that run (see
// README.md), but not a meaningful head-to-head comparison. This much
// smaller account/instrument universe exists solely to give the baseline
// detector a fair chance to warm up, for an actual quantified comparison.
SimulatorConfig baseline_comparison_config() {
    SimulatorConfig config;
    config.random_seed = 999;
    config.session_start_ns = kEpochAnchorNs;
    config.session_duration_ns = 600LL * 1'000'000'000;
    config.baseline_orders_per_second = 3.0;
    config.num_equity_instruments = 2;
    config.num_fx_instruments = 1;
    config.num_fixed_income_instruments = 0;
    config.num_independent_accounts = 15;
    config.num_linked_account_pairs = 5;
    config.wash_trade = {15, 0.5};
    config.spoofing_layering = {15, 0.5};
    config.marking_the_close = {15, 0.5};
    config.front_running = {15, 0.5};
    return config;
}

SimulatorConfig severity_gradient_config(double severity, uint64_t seed) {
    SimulatorConfig config;
    config.random_seed = seed;
    config.session_start_ns = kEpochAnchorNs;
    config.session_duration_ns = 300LL * 1'000'000'000;  // 5 synthetic minutes
    config.baseline_orders_per_second = 2.0;
    config.num_equity_instruments = 8;
    config.num_fx_instruments = 4;
    config.num_fixed_income_instruments = 3;
    config.num_independent_accounts = 150;
    config.num_linked_account_pairs = 30;
    config.wash_trade = {10, severity};
    config.spoofing_layering = {10, severity};
    config.marking_the_close = {10, severity};
    config.front_running = {10, severity};
    return config;
}

// MarkingTheCloseDetector dedups alerts per (instrument_id, account_id) for
// its whole lifetime (alerted_ in marking_the_close_detector.cpp) -- a
// genuine design choice ("don't re-alert the same account/instrument every
// trade in the closing window"), not a bug. But it means two DIFFERENT
// marking-the-close scenarios that happen to land on the same
// (instrument, account) pair within one simulation run will only ever
// produce one alert between them, which would show up as a false FN if
// left unexplained -- this is exactly the mechanism that produced the
// live-dashboard bug fixed earlier in this project. Counted and reported
// explicitly here so a depressed MTC recall number is never silently
// misread as a detector defect.
int count_mtc_key_collisions(const SimulationOutput& simulation) {
    std::unordered_set<std::string> seen_keys;
    std::unordered_map<std::string, std::string> scenario_by_key;
    int collisions = 0;
    for (const auto& execution : simulation.executions) {
        if (execution.ground_truth_label.pattern != AbusePattern::kMarkingTheClose) continue;
        const std::string key = execution.instrument_id + "|" + execution.account_id;
        auto it = scenario_by_key.find(key);
        if (it == scenario_by_key.end()) {
            scenario_by_key[key] = execution.ground_truth_label.scenario_id;
        } else if (it->second != execution.ground_truth_label.scenario_id) {
            if (seen_keys.insert(key).second) ++collisions;
        }
    }
    return collisions;
}

struct EvalRun {
    SimulationOutput simulation;
    tse::harness::ReplayResult replay;
    tse::harness::Universes universes;
};

EvalRun run_eval(const SimulatorConfig& config, const std::string& topic_label) {
    EvalRun run;
    run.simulation = tse::simulator::generate_simulation(config);
    run.replay = tse::harness::replay_through_kafka(run.simulation, kBrokers, unique_topic(topic_label));
    run.universes = tse::harness::build_universes(run.simulation);
    return run;
}

}  // namespace

int main() {
    std::printf("=== Phase 10 Evaluation Harness ===\n");
    std::printf("Replaying Phase 1 synthetic scenarios through Kafka (%s) into pipeline/'s real LivePipeline,\n",
                kBrokers);
    std::printf("wired with the five Phase 5 rule-based detectors (MlAnomalyDetector excluded -- see README.md).\n\n");

    SimulatorConfig config = main_eval_config();
    EvalRun run;
    try {
        run = run_eval(config, "main");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        return 1;
    }

    std::printf("--- Replay integrity ---\n");
    std::printf("  orders=%zu executions=%zu total_events_published=%llu replayed_from_kafka=%llu\n",
                run.simulation.orders.size(), run.simulation.executions.size(),
                static_cast<unsigned long long>(run.replay.events_total),
                static_cast<unsigned long long>(run.replay.events_replayed_from_kafka));
    std::printf("  events_processed_by_pipeline=%llu events_skipped_inconsistent=%llu ring_buffer_dropped=%llu\n\n",
                static_cast<unsigned long long>(run.replay.events_processed),
                static_cast<unsigned long long>(run.replay.events_skipped_inconsistent),
                static_cast<unsigned long long>(run.replay.ring_buffer_dropped));

    int mtc_collisions = count_mtc_key_collisions(run.simulation);
    if (mtc_collisions > 0) {
        std::printf(
            "  NOTE: %d MarkingTheCloseDetector scenario(s) share an (instrument,account) key with an earlier\n"
            "  scenario in this run. MarkingTheCloseDetector dedups by that key for its whole lifetime by design\n"
            "  (see marking_the_close_detector.cpp), so at most one of a colliding pair can ever fire -- any FN\n"
            "  attributable to this is a measurement artifact of this run's random draws, not a detector defect.\n\n",
            mtc_collisions);
    }

    std::printf("--- Per-detector precision / recall / F1 at operating threshold 0.5 ---\n");
    for (const auto& spec : tse::harness::pattern_aware_detector_specs()) {
        const auto& universe =
            tse::harness::select_universe(run.universes, tse::harness::detector_universe_kind(spec.detector_name));
        const auto& positive = tse::harness::positive_ids_for(universe, spec.target_pattern);
        auto matrix = tse::harness::compute_confusion_matrix(run.replay.alerts, spec.detector_name, 0.5,
                                                               universe.event_ids, positive);
        print_matrix_row(spec.detector_name + " (" + tse::simulator::to_string(spec.target_pattern) + ")", matrix);
    }
    std::printf("\n");

    std::printf("--- StatisticalBaselineDetector vs. each pattern-aware detector's own target pattern ---\n");
    {
        const auto& sb_universe = tse::harness::select_universe(run.universes, tse::harness::UniverseKind::kNewOrder);
        for (const auto& spec : tse::harness::pattern_aware_detector_specs()) {
            const auto& positive = tse::harness::positive_ids_for(sb_universe, spec.target_pattern);
            auto matrix = tse::harness::compute_confusion_matrix(run.replay.alerts, "StatisticalBaselineDetector", 0.5,
                                                                   sb_universe.event_ids, positive);
            print_matrix_row(std::string("StatisticalBaselineDetector (") + tse::simulator::to_string(spec.target_pattern) + ")",
                              matrix);
        }
        auto any_abuse = tse::harness::any_abuse_ids(sb_universe);
        auto matrix =
            tse::harness::compute_confusion_matrix(run.replay.alerts, "StatisticalBaselineDetector", 0.5,
                                                     sb_universe.event_ids, any_abuse);
        print_matrix_row("StatisticalBaselineDetector (any injected abuse)", matrix);
        if (matrix.tp == 0 && matrix.fp == 0) {
            std::printf(
                "  NOTE: StatisticalBaselineDetector never fired a single alert in this run. Root cause (measured\n"
                "  separately): only ~1.7%% of (account,instrument) keys in this %d-account x %d-instrument, 10-\n"
                "  minute session ever accumulate the %d prior same-key New orders its z-score needs before it can\n"
                "  evaluate anything at all -- see README.md. Not a detector bug; the section below re-runs the\n"
                "  same comparison against a much smaller account/instrument universe so the baseline gets a fair\n"
                "  chance to warm up.\n\n",
                config.num_independent_accounts + config.num_linked_account_pairs * 2,
                config.num_equity_instruments + config.num_fx_instruments + config.num_fixed_income_instruments, 5);
        }
    }
    std::printf("\n");

    std::printf(
        "--- StatisticalBaselineDetector vs. pattern-aware detectors, small account/instrument universe\n"
        "    (gives the baseline's per-key z-score enough repeated activity to actually warm up) ---\n");
    {
        SimulatorConfig cmp_config = baseline_comparison_config();
        EvalRun cmp_run;
        try {
            cmp_run = run_eval(cmp_config, "basecmp");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FATAL (baseline comparison run): %s\n", e.what());
            return 1;
        }
        std::printf("  accounts=%d instruments=%d orders=%zu executions=%zu\n",
                    cmp_config.num_independent_accounts + cmp_config.num_linked_account_pairs * 2,
                    cmp_config.num_equity_instruments + cmp_config.num_fx_instruments + cmp_config.num_fixed_income_instruments,
                    cmp_run.simulation.orders.size(), cmp_run.simulation.executions.size());
        const auto& sb_universe_cmp = tse::harness::select_universe(cmp_run.universes, tse::harness::UniverseKind::kNewOrder);
        for (const auto& spec : tse::harness::pattern_aware_detector_specs()) {
            const auto& target_universe =
                tse::harness::select_universe(cmp_run.universes, tse::harness::detector_universe_kind(spec.detector_name));
            const auto& target_positive = tse::harness::positive_ids_for(target_universe, spec.target_pattern);
            auto pattern_matrix = tse::harness::compute_confusion_matrix(cmp_run.replay.alerts, spec.detector_name, 0.5,
                                                                           target_universe.event_ids, target_positive);
            print_matrix_row(spec.detector_name + " (pattern-aware)", pattern_matrix);

            const auto& sb_positive = tse::harness::positive_ids_for(sb_universe_cmp, spec.target_pattern);
            auto sb_matrix = tse::harness::compute_confusion_matrix(cmp_run.replay.alerts, "StatisticalBaselineDetector", 0.5,
                                                                      sb_universe_cmp.event_ids, sb_positive);
            print_matrix_row("  vs. StatisticalBaselineDetector (naive)", sb_matrix);
        }
        auto any_abuse_cmp = tse::harness::any_abuse_ids(sb_universe_cmp);
        auto sb_any_matrix = tse::harness::compute_confusion_matrix(cmp_run.replay.alerts, "StatisticalBaselineDetector", 0.5,
                                                                      sb_universe_cmp.event_ids, any_abuse_cmp);
        print_matrix_row("StatisticalBaselineDetector (any injected abuse)", sb_any_matrix);
    }
    std::printf("\n");

    std::printf("--- Threshold sweep (0.0 .. 1.0 step 0.1), full confusion matrix at each point ---\n");
    std::vector<double> thresholds = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0};
    for (const auto& spec : tse::harness::pattern_aware_detector_specs()) {
        const auto& universe =
            tse::harness::select_universe(run.universes, tse::harness::detector_universe_kind(spec.detector_name));
        const auto& positive = tse::harness::positive_ids_for(universe, spec.target_pattern);
        std::printf(" %s (target=%s):\n", spec.detector_name.c_str(), tse::simulator::to_string(spec.target_pattern));
        auto sweep =
            tse::harness::threshold_sweep(run.replay.alerts, spec.detector_name, universe.event_ids, positive, thresholds);
        for (const auto& point : sweep) {
            char label[32];
            std::snprintf(label, sizeof(label), "threshold=%.1f", point.threshold);
            print_matrix_row(label, point.matrix);
        }
        std::printf("\n");
    }

    std::printf("--- Difficulty gradient: recall (threshold 0.5) as a function of Phase 1's severity ---\n");
    std::vector<double> severities = {0.1, 0.3, 0.5, 0.7, 0.9};
    std::printf("  %-12s", "severity");
    for (const auto& spec : tse::harness::pattern_aware_detector_specs()) std::printf(" %-24s", spec.detector_name.c_str());
    std::printf("\n");
    for (size_t i = 0; i < severities.size(); ++i) {
        double severity = severities[i];
        SimulatorConfig grad_config = severity_gradient_config(severity, 5000 + i);
        EvalRun grad_run;
        try {
            grad_run = run_eval(grad_config, "grad" + std::to_string(i));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FATAL (severity gradient, severity=%.1f): %s\n", severity, e.what());
            return 1;
        }
        std::printf("  %-12.1f", severity);
        for (const auto& spec : tse::harness::pattern_aware_detector_specs()) {
            const auto& universe = tse::harness::select_universe(grad_run.universes,
                                                                   tse::harness::detector_universe_kind(spec.detector_name));
            const auto& positive = tse::harness::positive_ids_for(universe, spec.target_pattern);
            auto matrix = tse::harness::compute_confusion_matrix(grad_run.replay.alerts, spec.detector_name, 0.5,
                                                                   universe.event_ids, positive);
            std::printf(" %-24s", fmt(matrix.recall()).c_str());
        }
        std::printf("\n");
    }

    std::printf("\n=== Done ===\n");
    return 0;
}
