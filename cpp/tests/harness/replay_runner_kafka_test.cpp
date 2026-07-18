#include "replay_runner.hpp"

#include <chrono>
#include <string>
#include <unordered_set>

#include <gtest/gtest.h>

#include "evaluation.hpp"
#include "ground_truth.hpp"
#include "kafka_producer.hpp"

using tse::simulator::AbusePattern;
using tse::simulator::SimulatorConfig;

namespace {

constexpr const char* kBrokers = "localhost:9092";

std::string unique_topic() {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "tse-harness-replay-test-" + std::to_string(now);
}

// Mirrors cpp/tests/ingestion/kafka_replay_test.cpp's own preflight: skip
// cleanly (not a failure) if no local broker is reachable, since this is an
// integration test against an external service (docker compose up -d
// kafka), not a pure unit test.
bool kafka_reachable() {
    tse::ingestion::KafkaProducer producer(kBrokers, "tse-harness-preflight");
    return producer.flush(5000);
}

// High severity, small counts -- deliberately obvious injected scenarios
// (a real detector should have no trouble catching these), not a
// statistically representative sample. Statistical realism at scale is
// cpp/harness/main.cpp's job; this test only proves the replay-through-
// Kafka plumbing produces alerts that genuinely trace back to the right
// ground truth, end to end, against a real broker and a real LivePipeline.
SimulatorConfig small_obvious_config() {
    SimulatorConfig config;
    config.random_seed = 12345;
    config.session_start_ns = 1'700'000'000'000'000'000LL;
    config.session_duration_ns = 60LL * 1'000'000'000;
    config.baseline_orders_per_second = 2.0;
    config.num_equity_instruments = 2;
    config.num_fx_instruments = 0;
    config.num_fixed_income_instruments = 0;
    config.num_independent_accounts = 20;
    config.num_linked_account_pairs = 5;
    config.wash_trade = {2, 0.9};
    config.spoofing_layering = {2, 0.9};
    config.marking_the_close = {2, 0.9};
    config.front_running = {2, 0.9};
    return config;
}

}  // namespace

TEST(ReplayRunnerKafkaTest, ReplayThroughRealKafkaFiresEachPatternAwareDetectorOnItsOwnTargetPattern) {
    if (!kafka_reachable()) {
        GTEST_SKIP() << "Kafka broker at " << kBrokers << " not reachable — run `docker compose up -d kafka` first.";
    }

    tse::simulator::SimulationOutput simulation = tse::simulator::generate_simulation(small_obvious_config());
    tse::harness::ReplayResult result = tse::harness::replay_through_kafka(simulation, kBrokers, unique_topic());

    // The replay must be complete and consistent -- no dropped/skipped
    // events -- or every number computed from it below is meaningless.
    EXPECT_EQ(result.events_total, result.events_replayed_from_kafka);
    EXPECT_EQ(result.events_processed, result.events_total);
    EXPECT_EQ(result.events_skipped_inconsistent, 0u);
    EXPECT_EQ(result.ring_buffer_dropped, 0u);
    ASSERT_FALSE(result.alerts.empty()) << "no detector fired anything against obviously-injected scenarios";

    tse::harness::Universes universes = tse::harness::build_universes(simulation);

    for (const auto& spec : tse::harness::pattern_aware_detector_specs()) {
        const auto& universe =
            tse::harness::select_universe(universes, tse::harness::detector_universe_kind(spec.detector_name));
        const auto& positive = tse::harness::positive_ids_for(universe, spec.target_pattern);
        ASSERT_FALSE(positive.empty()) << spec.detector_name << ": test config produced zero ground-truth events "
                                                                  "for its own target pattern -- test setup bug";

        // Threshold 0.0, not 0.5: this test's job is to prove the replay
        // plumbing correctly attributes a genuinely-fired Alert back to its
        // ground truth, not to re-litigate any particular downstream
        // operating threshold. A detector has already applied its own
        // internal bar before returning an Alert at all (alert.hpp: "each
        // detector applies its own configurable threshold internally and
        // only returns an Alert once score clears it") -- MarkingTheClose's
        // own default concentration_threshold is 0.4, and a scenario whose
        // concentration share only just clears that (diluted by baseline
        // noise in this small test config) can score e.g. 0.42, which a
        // uniform 0.5 evaluation cutoff would wrongly discard as "missed."
        // cpp/harness/main.cpp's full sweep is where threshold choice
        // actually matters and is reported at every point, not cherry-picked.
        tse::harness::ConfusionMatrix matrix =
            tse::harness::compute_confusion_matrix(result.alerts, spec.detector_name, 0.0, universe.event_ids, positive);

        EXPECT_GT(matrix.tp, 0u) << spec.detector_name << " caught none of its " << positive.size()
                                  << " obviously-injected (severity=0.9) " << tse::simulator::to_string(spec.target_pattern)
                                  << " events";
        // No fp==0 assertion here deliberately: with random baseline flow
        // drawing counterparties uniformly from an account pool that
        // includes the linked pairs, a baseline execution can by chance
        // land on two related accounts (or an equally-timed unrelated
        // sequence FrontRunningDetector's window happens to catch) and
        // produce a genuine, correctly-behaving false positive -- that's
        // real detector behavior against noisy data, not a bug, and
        // asserting it away here would just make this test seed-fragile.
        // cpp/harness/main.cpp's full sweep is where FP rates are actually
        // measured and reported.
    }
}

TEST(ReplayRunnerKafkaTest, EvidenceTextNeverEmbedsAScenarioId) {
    if (!kafka_reachable()) {
        GTEST_SKIP() << "Kafka broker at " << kBrokers << " not reachable — run `docker compose up -d kafka` first.";
    }

    // Structural check backing CLAUDE.md's data rule: fix::Order/Execution
    // (what actually crosses Kafka -- see ingestion_event.hpp) and Alert
    // (what a detector emits -- see alert.hpp) both have no
    // ground_truth_label field at all, so there's no *typed* field for a
    // label to leak through. The one place a label even conceivably could
    // leak despite that is a detector's free-text evidence string, if it
    // were ever built from anything other than live book/order state. This
    // test proves it isn't: every fired alert's evidence text is checked
    // against every real scenario_id this run actually generated (e.g.
    // "SCN-WASH-000001"), and none of them appear.
    tse::simulator::SimulationOutput simulation = tse::simulator::generate_simulation(small_obvious_config());
    tse::harness::ReplayResult result = tse::harness::replay_through_kafka(simulation, kBrokers, unique_topic());
    ASSERT_FALSE(result.alerts.empty());

    std::unordered_set<std::string> scenario_ids;
    for (const auto& order : simulation.orders) {
        if (!order.ground_truth_label.scenario_id.empty()) scenario_ids.insert(order.ground_truth_label.scenario_id);
    }
    for (const auto& execution : simulation.executions) {
        if (!execution.ground_truth_label.scenario_id.empty()) {
            scenario_ids.insert(execution.ground_truth_label.scenario_id);
        }
    }
    ASSERT_FALSE(scenario_ids.empty());

    for (const auto& alert : result.alerts) {
        for (const auto& scenario_id : scenario_ids) {
            EXPECT_EQ(alert.evidence.find(scenario_id), std::string::npos)
                << alert.detector_name << "'s evidence text embeds ground-truth scenario id " << scenario_id;
        }
    }
}
