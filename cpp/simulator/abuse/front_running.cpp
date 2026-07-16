#include "abuse/front_running.hpp"

#include <algorithm>
#include <cmath>

#include "random_utils.hpp"

namespace tse::simulator {

namespace {
constexpr const char* kSyntheticCounterparty = "MKT-COUNTERPARTY";
}  // namespace

ScenarioOutput generate_front_running_scenario(std::mt19937_64& rng, IdGenerator& order_id_gen,
                                                IdGenerator& trade_id_gen,
                                                const std::string& scenario_id,
                                                const Instrument& instrument,
                                                const Account& account_client,
                                                const Account& account_related, double base_price,
                                                int64_t anchor_time_ns, double severity,
                                                const std::string& venue) {
    GroundTruthLabel label{AbusePattern::kFrontRunning, scenario_id, severity};
    ScenarioOutput output;

    Side side = uniform_double(rng, 0.0, 1.0) < 0.5 ? Side::kBuy : Side::kSell;
    double direction_sign = side == Side::kBuy ? 1.0 : -1.0;

    double reference_price_rounded = std::round(base_price / instrument.tick_size) * instrument.tick_size;
    int64_t qty_client = uniform_int64(rng, 10, 40) * 100;
    int64_t qty_related = uniform_int64(rng, 1, 5) * 100;

    // Reaction delay: how quickly the related account trades after the
    // client order is placed. Small and roughly constant — it's the *lead
    // time before the client's fill* (below) that severity controls.
    int64_t reaction_delay_ns = uniform_int64(rng, 20'000'000, 150'000'000);
    int64_t related_fill_delay_ns = uniform_int64(rng, 10'000'000, 100'000'000);

    int64_t min_lead_ns = reaction_delay_ns + related_fill_delay_ns + 10'000'000;
    int64_t front_run_lead_ns = std::max<int64_t>(
        min_lead_ns, static_cast<int64_t>(lerp(20'000'000'000.0, 300'000'000.0, severity)) +
                         uniform_int64(rng, 0, 500'000'000));

    int64_t slip_ticks = uniform_int64(rng, 1, 3);
    double related_price = reference_price_rounded;
    double client_price = reference_price_rounded + direction_sign * static_cast<double>(slip_ticks) * instrument.tick_size;

    Order client_order;
    client_order.order_id = order_id_gen.next();
    client_order.account_id = account_client.account_id;
    client_order.instrument_id = instrument.instrument_id;
    client_order.side = side;
    client_order.price = client_price;
    client_order.qty = qty_client;
    client_order.order_type = OrderType::kLimit;
    client_order.timestamp_ns = anchor_time_ns;
    client_order.status = OrderStatus::kNew;
    client_order.venue = venue;
    client_order.ground_truth_label = label;
    output.orders.push_back(client_order);

    Order related_order;
    related_order.order_id = order_id_gen.next();
    related_order.account_id = account_related.account_id;
    related_order.instrument_id = instrument.instrument_id;
    related_order.side = side;
    related_order.price = related_price;
    related_order.qty = qty_related;
    related_order.order_type = OrderType::kLimit;
    related_order.timestamp_ns = anchor_time_ns + reaction_delay_ns;
    related_order.status = OrderStatus::kNew;
    related_order.venue = venue;
    related_order.ground_truth_label = label;
    output.orders.push_back(related_order);

    Execution related_execution;
    related_execution.trade_id = trade_id_gen.next();
    related_execution.order_id = related_order.order_id;
    related_execution.account_id = account_related.account_id;
    related_execution.instrument_id = instrument.instrument_id;
    related_execution.price = related_price;
    related_execution.qty = qty_related;
    related_execution.timestamp_ns = related_order.timestamp_ns + related_fill_delay_ns;
    related_execution.counterparty_account_id = kSyntheticCounterparty;
    related_execution.venue = venue;
    related_execution.ground_truth_label = label;
    output.executions.push_back(related_execution);

    int64_t client_fill_ts = anchor_time_ns + front_run_lead_ns;
    Execution client_execution;
    client_execution.trade_id = trade_id_gen.next();
    client_execution.order_id = client_order.order_id;
    client_execution.account_id = account_client.account_id;
    client_execution.instrument_id = instrument.instrument_id;
    client_execution.price = client_price;
    client_execution.qty = qty_client;
    client_execution.timestamp_ns = client_fill_ts;
    client_execution.counterparty_account_id = kSyntheticCounterparty;
    client_execution.venue = venue;
    client_execution.ground_truth_label = label;
    output.executions.push_back(client_execution);

    if (severity >= 0.5) {
        Side reversal_side = side == Side::kBuy ? Side::kSell : Side::kBuy;
        double reversal_price = client_price + direction_sign * instrument.tick_size *
                                                    static_cast<double>(uniform_int64(rng, 1, 2));

        Order reversal_order;
        reversal_order.order_id = order_id_gen.next();
        reversal_order.account_id = account_related.account_id;
        reversal_order.instrument_id = instrument.instrument_id;
        reversal_order.side = reversal_side;
        reversal_order.price = reversal_price;
        reversal_order.qty = qty_related;
        reversal_order.order_type = OrderType::kLimit;
        reversal_order.timestamp_ns = client_fill_ts + uniform_int64(rng, 50'000'000, 500'000'000);
        reversal_order.status = OrderStatus::kNew;
        reversal_order.venue = venue;
        reversal_order.ground_truth_label = label;
        output.orders.push_back(reversal_order);

        Execution reversal_execution;
        reversal_execution.trade_id = trade_id_gen.next();
        reversal_execution.order_id = reversal_order.order_id;
        reversal_execution.account_id = account_related.account_id;
        reversal_execution.instrument_id = instrument.instrument_id;
        reversal_execution.price = reversal_price;
        reversal_execution.qty = qty_related;
        reversal_execution.timestamp_ns = reversal_order.timestamp_ns + uniform_int64(rng, 10'000'000, 100'000'000);
        reversal_execution.counterparty_account_id = kSyntheticCounterparty;
        reversal_execution.venue = venue;
        reversal_execution.ground_truth_label = label;
        output.executions.push_back(reversal_execution);
    }

    return output;
}

}  // namespace tse::simulator
