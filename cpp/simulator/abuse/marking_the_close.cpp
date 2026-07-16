#include "abuse/marking_the_close.hpp"

#include <algorithm>
#include <cmath>

#include "random_utils.hpp"

namespace tse::simulator {

namespace {
constexpr const char* kSyntheticCounterparty = "MKT-COUNTERPARTY";
}  // namespace

ScenarioOutput generate_marking_the_close_scenario(std::mt19937_64& rng, IdGenerator& order_id_gen,
                                                    IdGenerator& trade_id_gen,
                                                    const std::string& scenario_id,
                                                    const Instrument& instrument,
                                                    const std::vector<Account>& accounts,
                                                    double base_price, double severity,
                                                    const std::string& venue) {
    GroundTruthLabel label{AbusePattern::kMarkingTheClose, scenario_id, severity};
    ScenarioOutput output;
    if (accounts.empty()) return output;

    // Higher severity => activity clusters tighter to the close, fewer
    // accounts involved (more concentrated), more trades, bigger price steps.
    double window_ns = lerp(300'000'000'000.0, 8'000'000'000.0, severity);
    int num_trades = 3 + static_cast<int>(std::llround(severity * 7.0));  // 3..10
    double step_ticks = lerp(0.5, 4.0, severity);
    double qty_multiplier = lerp(2.0, 6.0, severity);
    size_t accounts_used = static_cast<size_t>(
        std::max(1.0, std::round(lerp(3.0, 1.0, severity))));
    accounts_used = std::min(accounts_used, accounts.size());

    bool push_up = uniform_double(rng, 0.0, 1.0) < 0.5;
    double direction_sign = push_up ? 1.0 : -1.0;
    Side side = push_up ? Side::kBuy : Side::kSell;

    double reference_price_rounded = std::round(base_price / instrument.tick_size) * instrument.tick_size;
    int64_t close_time = instrument.session_close_ns;
    int64_t window_start = close_time - static_cast<int64_t>(window_ns);
    int64_t baseline_qty_unit = uniform_int64(rng, 2, 10) * 100;

    for (int i = 0; i < num_trades; ++i) {
        double frac = static_cast<double>(i + 1) / static_cast<double>(num_trades);
        int64_t ts = window_start + static_cast<int64_t>(frac * window_ns) -
                     uniform_int64(rng, 0, static_cast<int64_t>(window_ns / (2.0 * num_trades)) + 1);
        ts = std::min(ts, close_time - 1'000'000);

        const Account& account = accounts[static_cast<size_t>(i) % accounts_used];
        double price = reference_price_rounded +
                       direction_sign * static_cast<double>(i + 1) * step_ticks * instrument.tick_size;
        int64_t qty = static_cast<int64_t>(std::llround(static_cast<double>(baseline_qty_unit) * qty_multiplier));

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
