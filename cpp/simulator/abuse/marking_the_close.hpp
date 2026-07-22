#pragma once

#include <random>
#include <string>
#include <vector>

#include "abuse/wash_trade.hpp"  // ScenarioOutput
#include "id_generator.hpp"
#include "types.hpp"

namespace tse::simulator {

// Concentrated, price-aggressive trades in a window right before
// instrument.session_close_ns, pushing the closing price in one direction.
//
// severity in [0,1] controls: how close to the actual close the activity
// clusters (minutes vs. seconds), how many accounts are involved (spread out
// vs. concentrated in one), the number of trades, and how aggressively each
// trade steps the price.
//
// orders_per_second/instrument_count are the same values driving baseline
// flow density (SimulatorConfig::baseline_orders_per_second and the total
// instrument count) -- this scenario's own volume is sized relative to
// expected ambient closing-window volume computed from them, not a flat
// absolute constant, so its concentration share (what
// MarkingTheCloseDetector actually evaluates) stays realistic regardless of
// how dense the background baseline flow is. See marking_the_close.cpp's
// own comment for the exact formula and why.
ScenarioOutput generate_marking_the_close_scenario(std::mt19937_64& rng, IdGenerator& order_id_gen,
                                                    IdGenerator& trade_id_gen,
                                                    const std::string& scenario_id,
                                                    const Instrument& instrument,
                                                    const std::vector<Account>& accounts,
                                                    double base_price, double severity,
                                                    double orders_per_second, int instrument_count,
                                                    const std::string& venue);

}  // namespace tse::simulator
