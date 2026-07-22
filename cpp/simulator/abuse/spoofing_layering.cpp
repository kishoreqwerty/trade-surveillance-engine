#include "abuse/spoofing_layering.hpp"

#include <cmath>

#include "random_utils.hpp"

namespace tse::simulator {

namespace {
constexpr const char* kSyntheticCounterparty = "MKT-COUNTERPARTY";
constexpr int64_t kLayerStaggerNs = 20'000'000;  // 20ms between successive layer placements

// Anchored to SpoofingLayeringDetector::slow_time_in_book_ns's own 5s
// default (cpp/detectors/spoofing_layering_detector.hpp) -- not an
// independently-guessed bound. Real WRDS TAQ data cannot give a
// cancel-to-placement timing distribution directly (TAQ has no
// order-level cancel messages -- see calibration/PARAMETER_MAPPING.md's
// caveat), so this is a self-consistency fix, not a plug-in-real-
// percentiles one. The actual Phase 10 bug: the old bounds (60s..0.5s)
// only dropped below the detector's fixed 5s threshold above severity
// ~=0.92, making speed_score exactly 0 -- no signal at all -- across the
// bottom ~92% of the severity range, confirmed by a direct trace against
// a real replay (all 12 layers in a severity=0.9 scenario scored
// speed=0.0). See spoofing_layering_detector_test.cpp for the fix's
// verification.
constexpr int64_t kSpeedScoreAnchorNs = 5'000'000'000LL;  // == detector's slow_time_in_book_ns default

// Ticks of deliberate headroom between reference_price_rounded and where
// the first spoof layer starts -- exists so the move_score anchor
// mechanism below has real room to place a genuinely dominant order
// without ever risking crossing the scenario's own layers. Found
// necessary empirically: with layers starting at reference+1tick (the
// original spacing), the anchor's only safe, non-crossing position was
// reference+0, which real ambient baseline flow can and does beat (its
// own independently-drifting mid can easily be a few ticks past a
// scenario's static reference by the time the scenario fires, since
// reference_price_rounded is computed once, deterministically, with no
// dependency on session-elapsed time). Widening the gap is also more
// realistic, not less -- real spoofing layers sit several price levels
// from touch (see cpp/simulator/abuse/sarao_case.hpp's cited "at least
// three or four price levels from the best asking price"), not stacked
// immediately at the inside.
constexpr double kAnchorSafeZoneTicks = 8.0;
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

    // Bounds deliberately straddle kSpeedScoreAnchorNs: severity=0 dwells
    // at 1.8x the anchor (clearly slow -- speed_score reads ~0, an
    // unremarkable-looking cancel), severity=1 at 0.2x (clearly fast --
    // speed_score reads ~1), crossing the anchor itself at severity=0.5.
    // Tight +/-0.5s jitter (was +0-2s) so the jitter doesn't swamp a
    // range that's now itself only ~8s wide end to end.
    int64_t dwell_ns = static_cast<int64_t>(lerp(1.8 * kSpeedScoreAnchorNs, 0.2 * kSpeedScoreAnchorNs, severity)) +
                        uniform_int64(rng, -500'000'000, 500'000'000);
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
        layer.price = reference_price_rounded +
                       away_sign * (kAnchorSafeZoneTicks + static_cast<double>(i + 1)) * instrument.tick_size;
        layer.qty = layer_qty;
        layer.order_type = OrderType::kLimit;
        layer.timestamp_ns = anchor_time_ns + static_cast<int64_t>(i) * kLayerStaggerNs;
        layer.status = OrderStatus::kNew;
        layer.venue = venue;
        layer.ground_truth_label = label;
        layers.push_back(layer);
        output.orders.push_back(layer);
    }

    // Deterministically moves best_price(side_genuine) between the
    // layers' placement and cancellation -- what SpoofingLayeringDetector's
    // move_score reads -- rather than leaving that to chance depend on
    // whatever ambient baseline order flow happens to be doing on that
    // side and when. Found via a real regression: cpp/simulator/'s
    // baseline recalibration (Phase 11) changed the baseline price walk's
    // short-timescale statistics enough that a previously-reliable
    // ReplayRunnerKafkaTest scenario stopped firing, because move_score
    // had never actually been controlled by this scenario -- it was
    // incidentally cooperating with whatever baseline noise happened to
    // do. Same test-fragility class already caught elsewhere in this
    // project (Phase 6's unpaced producer test, Phase 1/5's own
    // true-positive-case isolation principle): a scenario asserting a
    // known pattern fires should not depend on unrelated randomness.
    //
    // Mechanism: place a "dominant" reference order on the genuine side
    // at reference_price_rounded itself -- the one price guaranteed safe
    // against ever crossing the spoofer's own layers regardless of
    // direction (layers sit strictly on the *away* side of reference;
    // this is the boundary, not past it) -- so it becomes
    // best_price(side_genuine) independent of ambient baseline state.
    // Partway through the dwell window (before any layer cancels),
    // withdraw it and replace with a "fallback" order genuinely further
    // from reference on the same side, which persists -- guaranteeing
    // best_price(side_genuine) reads as having moved in the direction
    // move_score checks for by the time the layers are cancelled, by
    // construction, not chance. A dedicated synthetic identity
    // (kSyntheticCounterparty), not the account under investigation --
    // this represents controlled market structure the scenario needs to
    // exist, not activity attributable to the spoofer.
    // Direction, worked through explicitly because it's easy to get
    // backwards (an earlier version of this fix did, and shipped a
    // "dominant" anchor that was less aggressive than real ambient
    // liquidity, silently defeating the whole mechanism): "aggressive" for
    // a resting order means HIGHEST price on the Buy side, LOWEST price on
    // the Sell side. The spoof layers occupy the away_sign direction from
    // reference (e.g. away_sign=+1 -> layers ABOVE reference -> side_spoof
    // is Sell -> side_genuine is Buy, where HIGHER is more aggressive --
    // the *same* away_sign direction, just short of the layers). So the
    // dominant anchor also goes in the away_sign direction (+away_sign),
    // using nearly the full safe zone; the fallback goes in the opposite
    // direction (-away_sign), close to reference -- deliberately less
    // aggressive, representing the "moved away" state.
    constexpr double kDominantAnchorOffsetTicks = kAnchorSafeZoneTicks - 1.0;  // near the safe-zone edge -- maximally aggressive
    constexpr double kFallbackAnchorOffsetTicks = 1.0;  // close to reference, on the opposite side -- deliberately unaggressive
    constexpr int64_t kAnchorQty = 300;

    Order anchor_dominant;
    anchor_dominant.order_id = order_id_gen.next();
    anchor_dominant.account_id = kSyntheticCounterparty;
    anchor_dominant.instrument_id = instrument.instrument_id;
    anchor_dominant.side = side_genuine;
    anchor_dominant.price = reference_price_rounded + away_sign * kDominantAnchorOffsetTicks * instrument.tick_size;
    anchor_dominant.qty = kAnchorQty;
    anchor_dominant.order_type = OrderType::kLimit;
    anchor_dominant.timestamp_ns = anchor_time_ns;
    anchor_dominant.status = OrderStatus::kNew;
    anchor_dominant.venue = venue;
    // Deliberately NOT `label`: this is market-structure scaffolding, not
    // the spoofing pattern itself, and cpp/harness/'s Phase 10 evaluation
    // identifies "ground truth SpoofingLayering New orders" purely by this
    // field -- tagging these with the scenario's own label would make them
    // count as positive-class events the detector can never actually fire
    // on (SpoofingLayeringDetector's alert.order_ids only ever names the
    // layer being cancelled), silently deflating measured recall. Left at
    // its default kBaseline sentinel, same as any other non-abuse order.
    output.orders.push_back(anchor_dominant);

    // Must land strictly before the EARLIEST layer cancel can possibly
    // occur (cancel_anchor_ts - cancel_cluster_jitter_ns/2 is that
    // earliest point, given the +/-jitter/2 draw each layer's own cancel
    // timestamp uses below) -- not just before all layers are placed.
    // Getting this wrong would silently reintroduce exactly the kind of
    // luck-dependent timing this fix exists to eliminate.
    int64_t cancel_anchor_ts = anchor_time_ns + dwell_ns;
    int64_t earliest_possible_layer_cancel_ts = cancel_anchor_ts - cancel_cluster_jitter_ns / 2;
    int64_t anchor_move_ts = anchor_time_ns + dwell_ns / 2;
    if (anchor_move_ts <= layers.back().timestamp_ns) anchor_move_ts = layers.back().timestamp_ns + 1'000'000;
    if (anchor_move_ts >= earliest_possible_layer_cancel_ts) {
        anchor_move_ts = earliest_possible_layer_cancel_ts - 1'000'000;
    }

    Order anchor_dominant_cancel = anchor_dominant;
    anchor_dominant_cancel.status = OrderStatus::kCancelled;
    anchor_dominant_cancel.timestamp_ns = anchor_move_ts;
    output.orders.push_back(anchor_dominant_cancel);

    Order anchor_fallback;
    anchor_fallback.order_id = order_id_gen.next();
    anchor_fallback.account_id = kSyntheticCounterparty;
    anchor_fallback.instrument_id = instrument.instrument_id;
    anchor_fallback.side = side_genuine;
    anchor_fallback.price = reference_price_rounded - away_sign * kFallbackAnchorOffsetTicks * instrument.tick_size;
    anchor_fallback.qty = kAnchorQty;
    anchor_fallback.order_type = OrderType::kLimit;
    anchor_fallback.timestamp_ns = anchor_move_ts + 1'000'000;
    anchor_fallback.status = OrderStatus::kNew;
    anchor_fallback.venue = venue;
    // Same reasoning as anchor_dominant above: left at the kBaseline default.
    output.orders.push_back(anchor_fallback);

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
