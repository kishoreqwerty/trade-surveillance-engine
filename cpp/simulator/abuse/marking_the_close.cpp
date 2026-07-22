#include "abuse/marking_the_close.hpp"

#include <algorithm>
#include <cmath>

#include "random_utils.hpp"

namespace tse::simulator {

namespace {
constexpr const char* kSyntheticCounterparty = "MKT-COUNTERPARTY";

// Matches MarkingTheCloseConfig::window_duration_ns's own default (300s,
// marking_the_close_detector.hpp) -- the detector's FIXED trailing window
// this scenario is actually evaluated against, regardless of where this
// scenario's own (severity-dependent, often much shorter) window_ns
// clusters its trades within it. Same anchoring principle as
// spoofing_layering.cpp's kSpeedScoreAnchorNs.
constexpr double kMtcWindowSecondsAnchor = 300.0;

// Mean qty of one baseline execution, conditional on an execution actually
// occurring: (0.60*mean_full_fill_qty + 0.15*mean_partial_fill_qty) / 0.75,
// using kTradeSizePercentiles' own trapezoidal mean (53.275) and Row 4's
// fill split (baseline_generator.cpp) -- both cross-referenced there so a
// future change to either doesn't silently go stale here.
constexpr double kMeanBaselineExecutionQty = 47.95;
constexpr double kBaselineExecutionProbability = 0.75;  // Row 4: 0.60 full-fill + 0.15 partial-fill
}  // namespace

ScenarioOutput generate_marking_the_close_scenario(std::mt19937_64& rng, IdGenerator& order_id_gen,
                                                    IdGenerator& trade_id_gen,
                                                    const std::string& scenario_id,
                                                    const Instrument& instrument,
                                                    const std::vector<Account>& accounts,
                                                    double base_price, double severity,
                                                    double orders_per_second, int instrument_count,
                                                    const std::string& venue) {
    GroundTruthLabel label{AbusePattern::kMarkingTheClose, scenario_id, severity};
    ScenarioOutput output;
    if (accounts.empty()) return output;

    // Higher severity => activity clusters tighter to the close, fewer
    // accounts involved (more concentrated), more trades, bigger price steps.
    //
    // accounts_used capped at 2, not 3: Phase 11.5's caller
    // (simulator.cpp) now passes a genuinely related pair
    // (account_registry.random_linked_pair()) so this scenario actually
    // exercises MarkingTheCloseDetector's beneficial-owner/linked-account
    // aggregation -- but the simulator's AccountRegistry only models
    // relatedness as PAIRS (one shared beneficial_owner_id per pair, fixed
    // at construction), not arbitrary-size groups. Extending it to support
    // 3-way linked groups is a bigger simulator-side data-model change than
    // this fix calls for; capping at 2 is the honest reflection of what's
    // actually supported, not a silent downgrade -- severity now spans
    // 1-2 accounts, not 1-3.
    double window_ns = lerp(300'000'000'000.0, 8'000'000'000.0, severity);
    int num_trades = 3 + static_cast<int>(std::llround(severity * 7.0));  // 3..10
    double step_ticks = lerp(0.5, 4.0, severity);
    size_t accounts_used = static_cast<size_t>(
        std::max(1.0, std::round(lerp(2.0, 1.0, severity))));
    accounts_used = std::min(accounts_used, accounts.size());

    // This scenario's total volume is sized relative to expected ambient
    // closing-window volume (computed from orders_per_second/
    // instrument_count), not a flat absolute constant -- see this file's
    // top-of-namespace comment for why.
    //
    // History (see PARAMETER_MAPPING.md for the full investigation):
    // originally, MarkingTheCloseDetector::check_account() checked
    // PER-ACCOUNT share (account_qty / total_qty), not this scenario's
    // combined share, so accounts_used splitting this scenario's volume
    // across multiple accounts capped each account's own share, as
    // total_scenario_qty -> infinity, at 1/accounts_used of the total (the
    // OTHER scenario accounts diluted it too). That was worked around by
    // framing the target as k/accounts_used, which cancelled the dependence
    // out algebraically -- but left a real, documented detector limitation:
    // splitting volume across 2+ accounts evaded the per-account check
    // entirely, at any realistic volume.
    //
    // Phase 11.5 closed that gap on the DETECTOR side instead
    // (MarkingTheCloseDetector now aggregates by beneficial-owner/linked-
    // account group, reusing AccountRegistry::is_related() -- see
    // marking_the_close_detector.cpp) and on the generator side (this
    // scenario's multi-account pool is now a genuinely related pair,
    // account_registry.random_linked_pair(), not independent accounts --
    // see simulator.cpp). With the detector summing the whole group's qty,
    // the 1/accounts_used ceiling no longer exists, so target_share is now
    // just k directly -- accounts_used=1 is unaffected either way (k/1 = k
    // algebraically, so removing the division changes nothing there).
    //
    // k's own range needed revisiting once it stopped being divided: the
    // original k = lerp(0.3, 0.85, severity) was chosen as a PER-ACCOUNT
    // target inside the old divided formula, where the real achieved share
    // never actually reached its nominal upper bound (0.85/accounts_used
    // <= 0.425 in practice). Used directly as the real group share, 0.85
    // would mean the scheme represents 85% of ALL window volume at the
    // "obvious" end of severity -- implausible near-total market
    // domination, not a realistic even-if-blatant case (checked against
    // market-structure reasoning, not a specific verified enforcement
    // citation -- none exists for an exact figure). Revised to
    // k = lerp(0.15, 0.65, severity): 15% (clearly subtle, well under this
    // detector's own 0.4 concentration_threshold) to 65% (dominant, but not
    // near-total). At severity=0.5, k=0.4 lands almost exactly on the
    // detector's own threshold -- an expected, not engineered-around,
    // consequence of this detector's hard-threshold (not continuous-score)
    // design: a linear severity dial crossing a fixed bar has to cross it
    // somewhere.
    double instruments_for_rate = std::max(1, instrument_count);
    double expected_ambient_qty = (orders_per_second / instruments_for_rate) * kBaselineExecutionProbability *
                                   kMeanBaselineExecutionQty * kMtcWindowSecondsAnchor;
    double target_share = lerp(0.15, 0.65, severity);
    double total_scenario_qty = target_share / (1.0 - target_share) * expected_ambient_qty;
    int64_t qty_per_trade = std::max<int64_t>(1, std::llround(total_scenario_qty / num_trades));

    bool push_up = uniform_double(rng, 0.0, 1.0) < 0.5;
    double direction_sign = push_up ? 1.0 : -1.0;
    Side side = push_up ? Side::kBuy : Side::kSell;

    double reference_price_rounded = std::round(base_price / instrument.tick_size) * instrument.tick_size;
    int64_t close_time = instrument.session_close_ns;
    int64_t window_start = close_time - static_cast<int64_t>(window_ns);

    for (int i = 0; i < num_trades; ++i) {
        double frac = static_cast<double>(i + 1) / static_cast<double>(num_trades);
        int64_t ts = window_start + static_cast<int64_t>(frac * window_ns) -
                     uniform_int64(rng, 0, static_cast<int64_t>(window_ns / (2.0 * num_trades)) + 1);
        ts = std::min(ts, close_time - 1'000'000);

        const Account& account = accounts[static_cast<size_t>(i) % accounts_used];
        double price = reference_price_rounded +
                       direction_sign * static_cast<double>(i + 1) * step_ticks * instrument.tick_size;
        int64_t qty = qty_per_trade;

        Order order;
        order.order_id = order_id_gen.next();
        order.account_id = account.account_id;
        order.instrument_id = instrument.instrument_id;
        order.side = side;
        order.price = price;
        order.qty = qty;
        order.order_type = OrderType::kLimit;
        order.timestamp_ns = ts;
        order.status = OrderStatus::kNew;
        order.venue = venue;
        order.ground_truth_label = label;
        output.orders.push_back(order);

        Execution execution;
        execution.trade_id = trade_id_gen.next();
        execution.order_id = order.order_id;
        execution.account_id = account.account_id;
        execution.instrument_id = instrument.instrument_id;
        execution.price = price;
        execution.qty = qty;
        execution.timestamp_ns = ts + uniform_int64(rng, 1'000'000, 20'000'000);
        execution.counterparty_account_id = kSyntheticCounterparty;
        execution.venue = venue;
        execution.ground_truth_label = label;
        output.executions.push_back(execution);
    }

    return output;
}

}  // namespace tse::simulator
