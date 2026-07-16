#pragma once

#include <random>
#include <string>

#include "abuse/wash_trade.hpp"  // ScenarioOutput
#include "id_generator.hpp"
#include "types.hpp"

namespace tse::simulator {

// One account places several large layered orders on one side of the book
// (inflating visible depth away from touch), then shortly after executes a
// smaller genuine order on the opposite side, then cancels all the layered
// orders. Mirrors the CFTC Sarao pattern: large resting orders create a false
// impression of depth to move price, genuine flow profits from the move, the
// spoof orders are pulled before they'd ever fill.
//
// severity in [0,1] controls: number of layers, layer size relative to
// baseline order size (% of visible depth), how short the dwell time is
// before cancellation, and how tightly the cancels cluster around the
// genuine trade (all more pronounced/obvious at higher severity).
ScenarioOutput generate_spoofing_layering_scenario(std::mt19937_64& rng, IdGenerator& order_id_gen,
                                                    IdGenerator& trade_id_gen,
                                                    const std::string& scenario_id,
                                                    const Instrument& instrument,
                                                    const Account& account, double base_price,
                                                    int64_t anchor_time_ns, double severity,
                                                    const std::string& venue);

}  // namespace tse::simulator
