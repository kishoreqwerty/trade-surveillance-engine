#pragma once

#include <random>
#include <string>

#include "abuse/wash_trade.hpp"  // ScenarioOutput
#include "id_generator.hpp"
#include "types.hpp"

namespace tse::simulator {

// A client places a large order; a related account (sharing a beneficial
// owner, e.g. a linked proprietary desk) trades ahead of it in the same
// direction, then the client order fills at a worse price reflecting the
// market impact the related account anticipated.
//
// severity in [0,1] controls: how short the lead time is between the related
// account's trade and the client's fill (tight coupling = obvious), and
// whether the related account reverses its position shortly after the
// client's fill to capture the move (included only at severity >= 0.5 — a
// clean round-trip is the more obviously suspicious variant).
ScenarioOutput generate_front_running_scenario(std::mt19937_64& rng, IdGenerator& order_id_gen,
                                                IdGenerator& trade_id_gen,
                                                const std::string& scenario_id,
                                                const Instrument& instrument,
                                                const Account& account_client,
                                                const Account& account_related, double base_price,
                                                int64_t anchor_time_ns, double severity,
                                                const std::string& venue);

}  // namespace tse::simulator
