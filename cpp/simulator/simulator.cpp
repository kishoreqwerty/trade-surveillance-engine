#include "simulator.hpp"

#include <algorithm>
#include <random>

#include "abuse/front_running.hpp"
#include "abuse/marking_the_close.hpp"
#include "abuse/spoofing_layering.hpp"
#include "abuse/wash_trade.hpp"
#include "account_registry.hpp"
#include "baseline_generator.hpp"
#include "id_generator.hpp"
#include "instrument_universe.hpp"
#include "random_utils.hpp"

namespace tse::simulator {

namespace {

constexpr const char* kVenue = "SIM";
constexpr int64_t kScenarioMarginNs = 300LL * 1'000'000'000;  // keep scenarios clear of session end

template <typename T>
void append(std::vector<T>& dst, std::vector<T>&& src) {
    dst.insert(dst.end(), std::make_move_iterator(src.begin()), std::make_move_iterator(src.end()));
}

bool by_timestamp_order(const Order& a, const Order& b) { return a.timestamp_ns < b.timestamp_ns; }
bool by_timestamp_execution(const Execution& a, const Execution& b) { return a.timestamp_ns < b.timestamp_ns; }

}  // namespace

SimulationOutput generate_simulation(const SimulatorConfig& config) {
    std::mt19937_64 rng(config.random_seed);

    // Deliberately independent from `rng` above (used for baseline flow),
    // not just a second draw from the same generator: abuse-scenario
    // generation must not depend on how many random draws baseline flow
    // happened to consume, which varies with baseline_orders_per_second.
    // A single shared stream meant every scenario's own random parameters
    // (which instrument, timing jitter, price-step direction, etc.) would
    // silently shift whenever the order rate changed, confounding any
    // attempt to isolate a rate-driven effect on detector recall from pure
    // RNG-state drift -- found while investigating SpoofingLayeringDetector's
    // noisy, non-monotonic recall trajectory across a Row 6 rate sweep (see
    // PARAMETER_MAPPING.md's Row 6 section). 0x9E3779B97F4A7C15ULL is the
    // standard golden-ratio hash-mixing constant, used only to decorrelate
    // the two seeds -- not a magic number specific to this project.
    std::mt19937_64 abuse_rng(config.random_seed ^ 0x9E3779B97F4A7C15ULL);

    int64_t session_end_ns = config.session_start_ns + config.session_duration_ns;

    std::vector<Instrument> instruments = build_instrument_universe(
        {config.num_equity_instruments, config.num_fx_instruments, config.num_fixed_income_instruments,
         session_end_ns});

    AccountRegistry account_registry({config.num_independent_accounts, config.num_linked_account_pairs}, rng);

    IdGenerator order_id_gen("ORD");
    IdGenerator trade_id_gen("EXE");
    IdGenerator wash_scenario_gen("SCN-WASH");
    IdGenerator spoof_scenario_gen("SCN-SPOOF");
    IdGenerator mtc_scenario_gen("SCN-MTC");
    IdGenerator fr_scenario_gen("SCN-FR");

    BaselineOutput baseline =
        generate_baseline_flow({config.baseline_orders_per_second, config.session_start_ns, session_end_ns},
                                instruments, account_registry.all(), rng, order_id_gen, trade_id_gen);

    std::vector<Order> orders = std::move(baseline.orders);
    std::vector<Execution> executions = std::move(baseline.executions);

    int64_t anchor_hi = std::max(config.session_start_ns, session_end_ns - kScenarioMarginNs);
    auto random_anchor = [&]() { return uniform_int64(abuse_rng, config.session_start_ns, anchor_hi); };

    for (int i = 0; i < config.wash_trade.count; ++i) {
        const Instrument& instrument = pick_random(instruments, abuse_rng);
        auto pair = account_registry.random_linked_pair(abuse_rng);
        auto scenario = generate_wash_trade_scenario(
            abuse_rng, order_id_gen, trade_id_gen, wash_scenario_gen.next(), instrument, pair.first, pair.second,
            reference_price(instrument), random_anchor(), config.wash_trade.severity, kVenue);
        append(orders, std::move(scenario.orders));
        append(executions, std::move(scenario.executions));
    }

    for (int i = 0; i < config.spoofing_layering.count; ++i) {
        const Instrument& instrument = pick_random(instruments, abuse_rng);
        const Account& account = account_registry.random_independent(abuse_rng);
        auto scenario = generate_spoofing_layering_scenario(
            abuse_rng, order_id_gen, trade_id_gen, spoof_scenario_gen.next(), instrument, account,
            reference_price(instrument), random_anchor(), config.spoofing_layering.severity, kVenue);
        append(orders, std::move(scenario.orders));
        append(executions, std::move(scenario.executions));
    }

    for (int i = 0; i < config.marking_the_close.count; ++i) {
        const Instrument& instrument = pick_random(instruments, abuse_rng);
        // Phase 11.5: a genuinely related pair, not independent accounts --
        // otherwise the scenario never exercises MarkingTheCloseDetector's
        // beneficial-owner/linked-account aggregation (Task 1), since
        // independent accounts are guaranteed unrelated by construction
        // (account_registry.cpp: unique beneficial_owner_id per account).
        // generate_marking_the_close_scenario()'s own accounts_used (capped
        // at 2, see marking_the_close.cpp) decides how many of these two
        // get used for a given severity; the second is simply unused when
        // accounts_used==1, at no cost.
        auto pair = account_registry.random_linked_pair(abuse_rng);
        std::vector<Account> pool = {pair.first, pair.second};
        auto scenario = generate_marking_the_close_scenario(
            abuse_rng, order_id_gen, trade_id_gen, mtc_scenario_gen.next(), instrument, pool,
            reference_price(instrument), config.marking_the_close.severity, config.baseline_orders_per_second,
            config.num_equity_instruments + config.num_fx_instruments + config.num_fixed_income_instruments,
            kVenue);
        append(orders, std::move(scenario.orders));
        append(executions, std::move(scenario.executions));
    }

    for (int i = 0; i < config.front_running.count; ++i) {
        const Instrument& instrument = pick_random(instruments, abuse_rng);
        auto pair = account_registry.random_linked_pair(abuse_rng);
        auto scenario = generate_front_running_scenario(
            abuse_rng, order_id_gen, trade_id_gen, fr_scenario_gen.next(), instrument, pair.first, pair.second,
            reference_price(instrument), random_anchor(), config.front_running.severity, kVenue);
        append(orders, std::move(scenario.orders));
        append(executions, std::move(scenario.executions));
    }

    std::stable_sort(orders.begin(), orders.end(), by_timestamp_order);
    std::stable_sort(executions.begin(), executions.end(), by_timestamp_execution);

    SimulationOutput output;
    output.instruments = std::move(instruments);
    output.accounts = account_registry.all();
    output.orders = std::move(orders);
    output.executions = std::move(executions);
    return output;
}

}  // namespace tse::simulator
