// Phase 11's CFTC Sarao validation: replays
// cpp/simulator/abuse/sarao_case.cpp's documented Layering Algorithm
// pattern through the real live pipeline -- the exact same Kafka-replay
// machinery Phase 10 built (replay_runner.hpp), not a separate check --
// and reports, honestly, whether SpoofingLayeringDetector fires and at
// what score, including if it doesn't fire cleanly. Per CLAUDE.md: "If
// the Sarao validation case doesn't fire cleanly, report that honestly
// with your best explanation rather than adjusting thresholds until it
// does" -- this program does not tune anything based on the result.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>

#include "abuse/sarao_case.hpp"
#include "json_output.hpp"
#include "replay_runner.hpp"
#include "simulator.hpp"

namespace {

constexpr const char* kBrokers = "localhost:9092";
constexpr double kAlertThreshold = 0.6;  // SpoofingLayeringDetector's own default -- see its config

std::string unique_topic() {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "tse-sarao-validation-" + std::to_string(now);
}

// Must match sarao_case.cpp's kSpooferAccount exactly. Not exposed via
// sarao_case.hpp (the builder's internal account naming isn't part of its
// public contract), so duplicated here deliberately rather than reached
// into -- this file is already wholly Sarao-specific.
constexpr const char* kSpooferAccount = "SARAO";

// SpoofingLayeringDetector only ever fires from handle_cancel() (see
// ground_truth.hpp's own comment on its universe), so the real
// "opportunities to fire" denominator is the actual count of cancels the
// spoofer's own account generates -- not every cancel in the scenario.
// sarao_case.cpp constructs two structurally different cancel groups: the
// SARAO account's layering-order cancels (the actual pattern under test,
// 5 cycles x 5 layers = 25) and a separate MKT-BIDS account's bid-retreat
// cancels (the simulated market's own reaction to the visible imbalance,
// 5 of them -- not spoofing activity, and not something
// SpoofingLayeringDetector has any reason to fire on). Counting all
// cancels indiscriminately here first (an earlier version of this
// function) silently produced 30, not 25 -- caught by comparing against
// this scenario's own documented "5 levels x 5 cycles" construction before
// shipping a wrong denominator into the JSON snapshot.
int count_cancels(const tse::simulator::SimulationOutput& simulation) {
    int count = 0;
    for (const auto& order : simulation.orders) {
        if (order.status == tse::simulator::OrderStatus::kCancelled && order.account_id == kSpooferAccount) ++count;
    }
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    std::string json_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--json" && i + 1 < argc) json_path = argv[++i];
    }

    tse::simulator::SaraoCaseOutput sarao = tse::simulator::build_sarao_case();

    tse::simulator::SimulationOutput simulation;
    simulation.orders = sarao.orders;
    simulation.executions = sarao.executions;

    std::printf("=== Phase 11: CFTC Sarao Layering Validation ===\n");
    std::printf(
        "Replaying the documented Layering Algorithm pattern (%zu orders, %zu executions) through\n"
        "Kafka into the real LivePipeline -- see cpp/simulator/abuse/sarao_case.hpp for exactly which\n"
        "numbers are cited to the CFTC complaint/press release vs. illustrative.\n\n",
        simulation.orders.size(), simulation.executions.size());

    tse::harness::ReplayResult result;
    try {
        result = tse::harness::replay_through_kafka(simulation, kBrokers, unique_topic());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        return 1;
    }

    std::printf("--- Replay integrity ---\n");
    std::printf(
        "  orders=%zu executions=%zu published=%llu replayed=%llu processed=%llu "
        "skipped_inconsistent=%llu dropped=%llu\n\n",
        simulation.orders.size(), simulation.executions.size(),
        static_cast<unsigned long long>(result.events_total),
        static_cast<unsigned long long>(result.events_replayed_from_kafka),
        static_cast<unsigned long long>(result.events_processed),
        static_cast<unsigned long long>(result.events_skipped_inconsistent),
        static_cast<unsigned long long>(result.ring_buffer_dropped));

    int spoofing_alerts = 0;
    double max_score = 0.0;
    for (const auto& alert : result.alerts) {
        if (alert.detector_name != "SpoofingLayeringDetector") continue;
        ++spoofing_alerts;
        if (alert.score > max_score) max_score = alert.score;
        std::printf("  FIRED: score=%.4f account=%s order_ids=[", alert.score,
                    alert.account_ids.empty() ? "?" : alert.account_ids[0].c_str());
        for (const auto& id : alert.order_ids) std::printf("%s,", id.c_str());
        std::printf("]\n    evidence=%s\n", alert.evidence.c_str());
    }

    int total_cancels = count_cancels(simulation);

    std::printf("\n--- Result ---\n");
    if (spoofing_alerts > 0) {
        std::printf(
            "SpoofingLayeringDetector FIRED %d time(s) against the Sarao layering pattern (of %d cancel "
            "opportunities), max score=%.4f (alert_threshold default=%.1f)\n",
            spoofing_alerts, total_cancels, max_score, kAlertThreshold);
    } else {
        std::printf("SpoofingLayeringDetector DID NOT FIRE against the Sarao layering pattern as constructed.\n");
    }

    if (!json_path.empty()) {
        using tse::harness::json_bool;
        using tse::harness::json_number;
        using tse::harness::JsonWriter;

        JsonWriter replay_integrity;
        replay_integrity.field("orders", json_number(static_cast<int64_t>(simulation.orders.size())));
        replay_integrity.field("executions", json_number(static_cast<int64_t>(simulation.executions.size())));
        replay_integrity.field("published", json_number(static_cast<int64_t>(result.events_total)));
        replay_integrity.field("replayed", json_number(static_cast<int64_t>(result.events_replayed_from_kafka)));
        replay_integrity.field("processed", json_number(static_cast<int64_t>(result.events_processed)));
        replay_integrity.field("skipped_inconsistent", json_number(static_cast<int64_t>(result.events_skipped_inconsistent)));
        replay_integrity.field("dropped", json_number(static_cast<int64_t>(result.ring_buffer_dropped)));

        JsonWriter root;
        root.field("generated_at_unix_ns", json_number(tse::harness::now_epoch_ns()));
        root.field("fired_count", json_number(static_cast<int64_t>(spoofing_alerts)));
        root.field("total_cancel_opportunities", json_number(static_cast<int64_t>(total_cancels)));
        root.field("max_score", json_number(max_score));
        root.field("alert_threshold", json_number(kAlertThreshold));
        root.field("fired_cleanly", json_bool(spoofing_alerts > 0));
        root.field("replay_integrity", replay_integrity.str());

        std::ofstream out(json_path);
        if (!out) {
            std::fprintf(stderr, "FATAL: could not open --json output path %s for writing\n", json_path.c_str());
            return 1;
        }
        out << root.str();
        std::printf("\nWrote JSON snapshot to %s\n", json_path.c_str());
    }

    std::printf("\n=== Done ===\n");
    return 0;
}
