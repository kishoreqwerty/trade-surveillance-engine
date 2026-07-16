#pragma once

#include <vector>

#include "types.hpp"

namespace tse::simulator {

struct InstrumentUniverseConfig {
    int num_equity{3};
    int num_fx{2};
    int num_fixed_income{2};
    int64_t session_close_ns{0};  // absolute epoch ns shared by all instruments in this session
};

std::vector<Instrument> build_instrument_universe(const InstrumentUniverseConfig& config);

// A representative "current market" price to anchor baseline flow and
// injected scenarios around, based on the instrument's asset class.
double reference_price(const Instrument& instrument);

}  // namespace tse::simulator
