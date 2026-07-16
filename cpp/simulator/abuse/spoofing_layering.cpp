#include "abuse/spoofing_layering.hpp"

#include <cmath>

#include "random_utils.hpp"

namespace tse::simulator {

namespace {
constexpr const char* kSyntheticCounterparty = "MKT-COUNTERPARTY";
constexpr int64_t kLayerStaggerNs = 20'000'000;  // 20ms between successive layer placements
}  // namespace

ScenarioOutput generate_spoofing_layering_scenario(std::mt19937_64& rng, IdGenerator& order_id_gen,
                                                    IdGenerator& trade_id_gen,
                                                    const std::string& scenario_id,
                                                    const Instrument& instrument,
                                                    const Account& account, double base_price,
                                                    int64_t anchor_time_ns, double severity,
                                                    const std::string& venue) {
    GroundTruthLabel label{AbusePattern::kSpoofingLayering, scenario_id, severity};

    int num_layers = 3 + static_cast<int>(std::llround(severity * 3.0));  // 3..6
    double qty_multiplier = lerp(1.5, 10.0, severity);
    int64_t baseline_qty_unit = uniform_int64(rng, 2, 10) * 100;
    int64_t layer_qty = static_cast<int64_t>(std::llround(static_cast<double>(baseline_qty_unit) * qty_multiplier));

    int64_t dwell_ns = static_cast<int64_t>(lerp(60'000'000'000.0, 500'000'000.0, severity)) +
                        uniform_int64(rng, 0, 2'000'000'000);
    int64_t cancel_cluster_jitter_ns = static_cast<int64_t>(lerp(5'000'000'000.0, 50'000'000.0, severity));

    bool spoof_sells = uniform_double(rng, 0.0, 1.0) < 0.5;
    Side side_spoof = spoof_sells ? Side::kSell : Side::kBuy;
    Side side_genuine = spoof_sells ? Side::kBuy : Side::kSell;
    double away_sign = spoof_sells ? 1.0 : -1.0;  // asks move up, bids move down as they layer further away

    double reference_price_rounded = std::round(base_price / instrument.tick_size) * instrument.tick_size;

    ScenarioOutput output;

    std::vector<Order> layers;
    layers.reserve(num_layers);
    for (int i = 0; i < num_layers; ++i) {
        Order layer;
        layer.order_id = order_id_gen.next();
        layer.account_id = account.account_id;
        layer.instrument_id = instrument.instrument_id;
        layer.side = side_spoof;
        layer.price = reference_price_rounded + away_sign * static_cast<double>(i + 1) * instrument.tick_size;
        layer.qty = layer_qty;
        layer.order_type = OrderType::kLimit;
        layer.timestamp_ns = anchor_time_ns + static_cast<int64_t>(i) * kLayerStaggerNs;
        layer.status = OrderStatus::kNew;
        layer.venue = venue;
        layer.ground_truth_label = label;
        layers.push_back(layer);
        output.orders.push_back(layer);
    }

    int64_t genuine_ts = anchor_time_ns + dwell_ns - uniform_int64(rng, 50'000'000, 300'000'000);
    int64_t last_layer_ts = layers.back().timestamp_ns;
    if (genuine_ts <= last_layer_ts) genuine_ts = last_layer_ts + 10'000'000;

    Order genuine;
    genuine.order_id = order_id_gen.next();
    genuine.account_id = account.account_id;
    genuine.instrument_id = instrument.instrument_id;
    genuine.side = side_genuine;
    genuine.price = reference_price_rounded - away_sign * instrument.tick_size;  // favorable, on the profiting side
    genuine.qty = uniform_int64(rng, 1, 5) * 100;
    genuine.order_type = OrderType::kLimit;
    genuine.timestamp_ns = genuine_ts;
    genuine.status = OrderStatus::kNew;
    genuine.venue = venue;
    genuine.ground_truth_label = label;
    output.orders.push_back(genuine);

    Execution genuine_execution;
    genuine_execution.trade_id = trade_id_gen.next();
    genuine_execution.order_id = genuine.order_id;
    genuine_execution.account_id = account.account_id;
    genuine_execution.instrument_id = instrument.instrument_id;
    genuine_execution.price = genuine.price;
    genuine_execution.qty = genuine.qty;
    genuine_execution.timestamp_ns = genuine_ts + uniform_int64(rng, 10'000'000, 100'000'000);
    genuine_execution.counterparty_account_id = kSyntheticCounterparty;
    genuine_execution.venue = venue;
    genuine_execution.ground_truth_label = label;
    output.executions.push_back(genuine_execution);

    int64_t cancel_anchor_ts = anchor_time_ns + dwell_ns;
    for (const auto& layer : layers) {
        Order cancel = layer;
        cancel.status = OrderStatus::kCancelled;
        cancel.timestamp_ns = std::max(layer.timestamp_ns + 1'000'000,
                                        cancel_anchor_ts + uniform_int64(rng, -cancel_cluster_jitter_ns / 2,
                                                                          cancel_cluster_jitter_ns / 2));
        output.orders.push_back(cancel);
    }

    return output;
}

}  // namespace tse::simulator
