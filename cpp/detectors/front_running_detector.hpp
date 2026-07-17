#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "i_detector.hpp"

namespace tse::detectors {

struct FrontRunningConfig {
    // How far back to look for a smaller related-account order that
    // preceded a large incoming order.
    int64_t lookback_window_ns{2'000'000'000LL};  // 2s

    // An incoming New order must be at least this size to be treated as
    // "client flow" worth front-running.
    int64_t min_large_qty_threshold{1000};

    // The earlier order must be no larger than this fraction of the large
    // order's size -- front-running is characterized by a small position
    // taken ahead of anticipated price impact from the larger order, not
    // two comparably-sized orders that happen to be sequenced closely.
    double max_leader_to_large_size_ratio{0.2};
};

// "Related-account sequencing ahead of client flow" (build guide, Phase
// 5): fires when a large incoming New order from account A is preceded,
// within lookback_window_ns and on the same instrument and side, by a
// smaller New order from a *related* account B (AccountRegistry::is_related
// -- same beneficial owner or an explicit link; B == A is deliberately
// excluded, since an account "front-running" its own order isn't a
// meaningful concept).
//
// Reacts only to the Order arm's New status -- this is a statement about
// *order placement sequencing*, matching "sequencing" in the build guide's
// own phrasing, not about fills. A deterministic rule (score == 1.0 when
// it fires), not a heuristic like SpoofingLayeringDetector -- the
// combination of "related account" + "smaller size" + "immediately
// preceding" is already a conjunction of three independent conditions,
// each already binary/threshold-based, so a further continuous confidence
// score would just be re-deriving the same three yes/no facts without
// adding real information.
//
// Maintains a per (instrument, side) rolling window of recent New orders
// (pruned to lookback_window_ns on every New), which is what lets a large
// order be matched against something that arrived microseconds or seconds
// earlier without re-scanning the whole order history.
class FrontRunningDetector : public IDetector {
public:
    explicit FrontRunningDetector(FrontRunningConfig config = {});

    std::vector<Alert> evaluate(const tse::orderbook::OrderBook& book, const DetectorEvent& incoming,
                                 const AccountRegistry& accounts) override;

    std::string name() const override { return "FrontRunningDetector"; }

private:
    struct RecentOrder {
        std::string account_id;
        std::string order_id;
        int64_t qty{0};
        int64_t timestamp_ns{0};
    };

    std::vector<Alert> handle_new(const tse::fix::Order& order, const AccountRegistry& accounts);

    FrontRunningConfig config_;
    std::unordered_map<std::string, std::vector<RecentOrder>> recent_by_key_;  // key: instrument_id + "|" + side
};

}  // namespace tse::detectors
