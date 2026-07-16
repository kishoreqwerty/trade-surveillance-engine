#pragma once

#include <cstdint>
#include <vector>

#include "types.hpp"

namespace tse::simulator {

struct AbusePatternConfig {
    int count{0};          // number of scenario instances to inject
    double severity{0.5};  // 0.0 (subtle) .. 1.0 (obvious), applied to every instance
};

struct SimulatorConfig {
    uint64_t random_seed{42};
    int64_t session_start_ns{0};
    int64_t session_duration_ns{6LL * 3600 * 1'000'000'000};  // 6h synthetic session
    double baseline_orders_per_second{5.0};

    int num_equity_instruments{3};
    int num_fx_instruments{2};
    int num_fixed_income_instruments{2};

    int num_independent_accounts{40};
    int num_linked_account_pairs{8};

    AbusePatternConfig wash_trade;
    AbusePatternConfig spoofing_layering;
    AbusePatternConfig marking_the_close;
    AbusePatternConfig front_running;
};

struct SimulationOutput {
    std::vector<Instrument> instruments;
    std::vector<Account> accounts;
    std::vector<Order> orders;          // time-sorted
    std::vector<Execution> executions;  // time-sorted
};

// Generates one synthetic trading session: realistic multi-asset baseline
// order flow plus the configured number of injected abuse scenarios per
// pattern, each carrying a ground_truth_label. Deterministic for a fixed
// random_seed.
SimulationOutput generate_simulation(const SimulatorConfig& config);

}  // namespace tse::simulator
