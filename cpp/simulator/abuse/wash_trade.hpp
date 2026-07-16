#pragma once

#include <random>
#include <string>
#include <vector>

#include "id_generator.hpp"
#include "types.hpp"

namespace tse::simulator {

struct ScenarioOutput {
    std::vector<Order> orders;
    std::vector<Execution> executions;
};

// Generates a matched pair of opposite-side orders between two linked
// accounts that execute against each other, netting to ~zero net position
// change for the shared beneficial owner.
//
// severity in [0,1]: higher severity means the two legs match more exactly
// in price/qty and land closer together in time (an obvious wash trade);
// lower severity spreads the legs out in time and adds price/qty noise (a
// subtler one that still nets the position but looks more like independent
// activity).
ScenarioOutput generate_wash_trade_scenario(std::mt19937_64& rng, IdGenerator& order_id_gen,
                                             IdGenerator& trade_id_gen,
                                             const std::string& scenario_id,
                                             const Instrument& instrument, const Account& account_a,
                                             const Account& account_b, double base_price,
                                             int64_t anchor_time_ns, double severity,
                                             const std::string& venue);

}  // namespace tse::simulator
