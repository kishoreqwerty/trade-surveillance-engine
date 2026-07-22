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
#include <string>

#include "abuse/sarao_case.hpp"
#include "replay_runner.hpp"
#include "simulator.hpp"

namespace {

constexpr const char* kBrokers = "localhost:9092";

std::string unique_topic() {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "tse-sarao-validation-" + std::to_string(now);
}

}  // namespace

int main() {
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

    std::printf("\n--- Result ---\n");
    if (spoofing_alerts > 0) {
        std::printf(
            "SpoofingLayeringDetector FIRED %d time(s) against the Sarao layering pattern, "
            "max score=%.4f (alert_threshold default=0.6)\n",
            spoofing_alerts, max_score);
    } else {
        std::printf("SpoofingLayeringDetector DID NOT FIRE against the Sarao layering pattern as constructed.\n");
    }

    std::printf("\n=== Done ===\n");
    return 0;
}
