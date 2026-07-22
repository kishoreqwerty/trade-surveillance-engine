#include "abuse/sarao_case.hpp"

#include <string>

namespace tse::simulator {

namespace {
constexpr const char* kInstrumentId = "ES";  // CME E-mini S&P 500 futures
constexpr const char* kVenue = "CME";
constexpr double kTickSize = 0.25;                     // real CME contract spec
constexpr double kIllustrativeReferencePrice = 1150.00;  // see sarao_case.hpp's ILLUSTRATIVE note
constexpr const char* kSpooferAccount = "SARAO";
constexpr const char* kBidLiquidityAccount = "MKT-BIDS";
constexpr const char* kScenarioId = "CFTC-SARAO-2010-05-06";

constexpr int kLayersPerCycle = 5;                        // cited: "four to six" -- using 5
constexpr int64_t kLayerLots[2] = {188, 289};             // cited: "188-and-289-lot" technique
constexpr int kMinLevelsFromBestAsk = 3;                  // cited: "at least three or four price levels"
constexpr int kNumCycles = 5;                             // illustrative count of "several times"
constexpr int64_t kTotalWindowNs = (2LL * 3600 + 15 * 60) * 1'000'000'000LL;  // cited: "over two hours"
constexpr int64_t kLayerStaggerNs = 500'000'000LL;        // illustrative: near-simultaneous placement
constexpr int64_t kLayerDwellNs = 8'000'000'000LL;        // illustrative: rapid dwell before cancel
// Comfortably after the last layer's placement (5 layers * 0.5s stagger =
// 2.0s) so the bid retreat unambiguously follows every layer's placement
// in timestamp order, not just in insertion order.
constexpr int64_t kBidPulldownDelayNs = 4'000'000'000LL;
}  // namespace

SaraoCaseOutput build_sarao_case(int64_t anchor_time_ns) {
    SaraoCaseOutput output;
    GroundTruthLabel label{AbusePattern::kSpoofingLayering, kScenarioId, 1.0};

    int order_counter = 0;
    auto next_order_id = [&] { return std::string("SARAO-ORD-") + std::to_string(++order_counter); };

    double best_ask_anchor = kIllustrativeReferencePrice + kTickSize;
    double current_bid_price = kIllustrativeReferencePrice;

    auto make_bid = [&](int64_t ts, double price) {
        Order bid;
        bid.order_id = next_order_id();
        bid.account_id = kBidLiquidityAccount;
        bid.instrument_id = kInstrumentId;
        bid.side = Side::kBuy;
        bid.price = price;
        bid.qty = 50;
        bid.order_type = OrderType::kLimit;
        bid.timestamp_ns = ts;
        bid.status = OrderStatus::kNew;
        bid.venue = kVenue;
        bid.ground_truth_label = label;
        return bid;
    };

    // Initial resting bid liquidity so the first cycle's move_score has a
    // real "best bid at placement" to compare against.
    Order current_bid = make_bid(anchor_time_ns, current_bid_price);
    output.orders.push_back(current_bid);

    const int64_t cycle_spacing_ns = kTotalWindowNs / kNumCycles;

    for (int cycle = 0; cycle < kNumCycles; ++cycle) {
        const int64_t cycle_start_ns = anchor_time_ns + static_cast<int64_t>(cycle) * cycle_spacing_ns;

        // "four to six exceptionally large sell orders into the visible
        // ... order book," "one price level from the other," kept "at
        // least three or four price levels from the best asking price."
        std::vector<Order> this_cycle_layers;
        for (int layer = 0; layer < kLayersPerCycle; ++layer) {
            Order sell;
            sell.order_id = next_order_id();
            sell.account_id = kSpooferAccount;
            sell.instrument_id = kInstrumentId;
            sell.side = Side::kSell;
            sell.price = best_ask_anchor + static_cast<double>(kMinLevelsFromBestAsk + layer) * kTickSize;
            sell.qty = kLayerLots[layer % 2];  // alternates the two cited real sizes
            sell.order_type = OrderType::kLimit;
            sell.timestamp_ns = cycle_start_ns + static_cast<int64_t>(layer) * kLayerStaggerNs;
            sell.status = OrderStatus::kNew;
            sell.venue = kVenue;
            sell.ground_truth_label = label;
            output.orders.push_back(sell);
            this_cycle_layers.push_back(sell);
        }

        // The market's own reaction to the visible imbalance: the best
        // bid retreats one tick partway through the layer orders' dwell
        // -- see sarao_case.hpp's ILLUSTRATIVE note. Cancels the prior
        // resting bid before placing the lower one, so the live OrderBook
        // (not just a local variable) actually reflects a declining best
        // bid -- what SpoofingLayeringDetector's move_score reads.
        Order cancel_old_bid = current_bid;
        cancel_old_bid.status = OrderStatus::kCancelled;
        cancel_old_bid.timestamp_ns = cycle_start_ns + kBidPulldownDelayNs;
        output.orders.push_back(cancel_old_bid);

        current_bid_price -= kTickSize;
        current_bid = make_bid(cycle_start_ns + kBidPulldownDelayNs + 1'000'000LL, current_bid_price);
        output.orders.push_back(current_bid);

        // "the vast majority of the Layering Algorithm orders were
        // canceled without resulting in any transactions" -- every layer
        // in this scenario cancels; none fill.
        for (const Order& layer_order : this_cycle_layers) {
            Order cancel = layer_order;
            cancel.status = OrderStatus::kCancelled;
            cancel.timestamp_ns = layer_order.timestamp_ns + kLayerDwellNs;
            output.orders.push_back(cancel);
        }
    }

    return output;
}

}  // namespace tse::simulator
