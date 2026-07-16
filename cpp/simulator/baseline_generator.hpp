#pragma once

#include <random>
#include <vector>

#include "id_generator.hpp"
#include "types.hpp"

namespace tse::simulator {

struct BaselineConfig {
    double orders_per_second{5.0};
    int64_t start_time_ns{0};
    int64_t end_time_ns{0};
};

struct BaselineOutput {
    std::vector<Order> orders;
    std::vector<Execution> executions;
};

// Generates realistic, non-abusive multi-asset order flow: Poisson arrivals
// spread across the given instruments/accounts, orders priced near a
// per-instrument random-walk mid, most either filled (fully or partially)
// against another independent account shortly after, or cancelled after a
// dwell period. Every event carries the kBaseline ground-truth sentinel.
BaselineOutput generate_baseline_flow(const BaselineConfig& config,
                                       const std::vector<Instrument>& instruments,
                                       const std::vector<Account>& accounts, std::mt19937_64& rng,
                                       IdGenerator& order_id_gen, IdGenerator& trade_id_gen);

}  // namespace tse::simulator
