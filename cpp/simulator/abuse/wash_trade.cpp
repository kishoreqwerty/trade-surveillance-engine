#include "abuse/wash_trade.hpp"

#include <algorithm>
#include <cmath>

#include "random_utils.hpp"

namespace tse::simulator {

ScenarioOutput generate_wash_trade_scenario(std::mt19937_64& rng, IdGenerator& order_id_gen,
                                             IdGenerator& trade_id_gen,
                                             const std::string& scenario_id,
                                             const Instrument& instrument, const Account& account_a,
                                             const Account& account_b, double base_price,
                                             int64_t anchor_time_ns, double severity,
                                             const std::string& venue) {
    GroundTruthLabel label{AbusePattern::kWashTrade, scenario_id, severity};

    bool a_buys = uniform_double(rng, 0.0, 1.0) < 0.5;
    Side side_a = a_buys ? Side::kBuy : Side::kSell;
    Side side_b = a_buys ? Side::kSell : Side::kBuy;

    int64_t qty_a = uniform_int64(rng, 2, 20) * 100;

    // Higher severity => tighter qty/price match and shorter leg gap (more
    // obviously a matched wash trade); lower severity => noisier and slower.
    double qty_noise_frac = lerp(0.20, 0.0, severity);
    double price_noise_ticks = lerp(3.0, 0.0, severity);
    int64_t leg_gap_ns = static_cast<int64_t>(lerp(30'000'000'000.0, 200'000'000.0, severity)) +
                          uniform_int64(rng, 0, 500'000'000);

    int64_t qty_b = static_cast<int64_t>(std::llround(
        static_cast<double>(qty_a) * (1.0 + uniform_double(rng, -qty_noise_frac, qty_noise_frac))));
    qty_b = std::max<int64_t>(1, qty_b);

    double price_a = std::round(base_price / instrument.tick_size) * instrument.tick_size;
    double price_offset = uniform_double(rng, -price_noise_ticks, price_noise_ticks) * instrument.tick_size;
    double price_b = price_a + price_offset;

    Order order_a;
    order_a.order_id = order_id_gen.next();
    order_a.account_id = account_a.account_id;
    order_a.instrument_id = instrument.instrument_id;
    order_a.side = side_a;
    order_a.price = price_a;
    order_a.qty = qty_a;
    order_a.order_type = OrderType::kLimit;
    order_a.timestamp_ns = anchor_time_ns;
    order_a.status = OrderStatus::kNew;
    order_a.venue = venue;
    order_a.ground_truth_label = label;

    Order order_b;
    order_b.order_id = order_id_gen.next();
    order_b.account_id = account_b.account_id;
    order_b.instrument_id = instrument.instrument_id;
    order_b.side = side_b;
    order_b.price = price_b;
    order_b.qty = qty_b;
    order_b.order_type = OrderType::kLimit;
    order_b.timestamp_ns = anchor_time_ns + leg_gap_ns;
    order_b.status = OrderStatus::kNew;
    order_b.venue = venue;
    order_b.ground_truth_label = label;

    int64_t trade_time_ns = order_b.timestamp_ns + uniform_int64(rng, 10'000'000, 200'000'000);
    int64_t filled_qty = std::min(qty_a, qty_b);

    Execution execution_a;
    execution_a.trade_id = trade_id_gen.next();
    execution_a.order_id = order_a.order_id;
    execution_a.account_id = account_a.account_id;
    execution_a.instrument_id = instrument.instrument_id;
    execution_a.price = price_a;
    execution_a.qty = filled_qty;
    execution_a.timestamp_ns = trade_time_ns;
    execution_a.counterparty_account_id = account_b.account_id;
    execution_a.venue = venue;
    execution_a.ground_truth_label = label;

    Execution execution_b;
    execution_b.trade_id = trade_id_gen.next();
    execution_b.order_id = order_b.order_id;
    execution_b.account_id = account_b.account_id;
    execution_b.instrument_id = instrument.instrument_id;
    execution_b.price = price_a;
    execution_b.qty = filled_qty;
    execution_b.timestamp_ns = trade_time_ns;
    execution_b.counterparty_account_id = account_a.account_id;
    execution_b.venue = venue;
    execution_b.ground_truth_label = label;

    ScenarioOutput output;
    output.orders = {order_a, order_b};
    output.executions = {execution_a, execution_b};
    return output;
}

}  // namespace tse::simulator
