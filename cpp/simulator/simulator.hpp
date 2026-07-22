#pragma once

#include <cstdint>
#include <vector>

#include "types.hpp"

namespace tse::simulator {

// Empirical order-arrival-rate proxy, WRDS TAQ (calibration/output/
// stylized_facts.json, order_arrival_rate_proxy_per_second.value,
// same corrected NBBO/sym_suffix basis and n=1,417,165 trades +
// 2,186,052 quotes as the other calibrated rows), EQUITY ONLY: (trades +
// quote updates) per symbol, divided by the actual observed pull window
// (~90min, derived from the data's own timestamps -- see
// PARAMETER_MAPPING.md's "Known pitfalls" for a real bug found and fixed
// here: an earlier version of this script divided by a hardcoded 6.5h
// session length against 90min-only data, understating the rate 4.33x).
//
// This is a PER-INSTRUMENT rate. baseline_generator.cpp's Poisson process
// runs at one flat rate (SimulatorConfig::baseline_orders_per_second) over
// the WHOLE instrument universe, then picks the instrument for each event
// uniformly at random (pick_random() in random_utils.hpp -- genuinely
// uniform, no asset-class weighting) -- so the realized per-instrument
// rate is baseline_orders_per_second / total_instrument_count, for EVERY
// instrument alike, equity or not. Multiplying this constant by the total
// instrument count therefore matches today's existing dilution behavior
// exactly, not a new mismatch on top of it: FX/fixed-income instruments
// already share whatever rate equities get (no per-asset-class rate has
// ever existed in this generator), so they end up generating orders at
// the equity-calibrated rate not because that's realistic for FX/fixed
// income -- it isn't, and there's no TAQ equivalent to check it against
// either way -- but because it's what today's uniform-selection mechanism
// already does, just scaled to a real number instead of a guessed one.
// Asset-class-differentiated rates remain out of scope (same position as
// "What's independent of WRDS entirely": FX/fixed-income stay
// uncalibrated).
constexpr double kEmpiricalOrderArrivalRatePerInstrumentPerSecond = 16.69;

struct AbusePatternConfig {
    int count{0};          // number of scenario instances to inject
    double severity{0.5};  // 0.0 (subtle) .. 1.0 (obvious), applied to every instance
};

struct SimulatorConfig {
    uint64_t random_seed{42};
    int64_t session_start_ns{0};
    int64_t session_duration_ns{6LL * 3600 * 1'000'000'000};  // 6h synthetic session

    int num_equity_instruments{3};
    int num_fx_instruments{2};
    int num_fixed_income_instruments{2};

    // Default instrument count above is 3+2+2=7 -- matches this default.
    // Callers that override the instrument counts should override this
    // too (harness/main.cpp does); it does not recompute itself.
    double baseline_orders_per_second{kEmpiricalOrderArrivalRatePerInstrumentPerSecond * 7};

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
