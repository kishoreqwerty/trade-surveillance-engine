#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "i_detector.hpp"

namespace tse::detectors {

struct FrontRunningConfig {
    // How far after a large related-account order was placed to keep
    // watching for a smaller follow-on order reacting to it.
    int64_t lookback_window_ns{2'000'000'000LL};  // 2s

    // A prior order must be at least this size to be treated as "client
    // flow" worth front-running.
    int64_t min_large_qty_threshold{1000};

    // The later, reacting order must be no larger than this fraction of
    // the large order's size -- front-running is characterized by a small
    // position taken ahead of anticipated price impact from the larger
    // order, not two comparably-sized orders that happen to be sequenced
    // closely.
    double max_leader_to_large_size_ratio{0.2};
};

// "Related-account sequencing ahead of client flow" (build guide, Phase
// 5): fires when a smaller New order from a *related* account B
// (AccountRegistry::is_related -- same beneficial owner or an explicit
// link; B == A is deliberately excluded, since an account "front-running"
// its own order isn't a meaningful concept) arrives within
// lookback_window_ns AFTER a large New order was already placed by
// account A on the same instrument and side.
//
// This direction -- reacting-order-after-large-order, not before -- is
// deliberate and matches the standard regulatory definition of
// front-running: trading ahead of a *known, pending* order using advance
// knowledge that it exists, not pre-positioning before the order itself
// has even been placed (which would require knowledge of a not-yet-real
// event, a materially different and far rarer claim). The large order
// becomes "known" to a related party the moment it's placed; what makes
// it front-running is that the related account reacts and gets filled
// quickly, well before the large order's own -- typically much later --
// fill and price impact. cpp/simulator/abuse/front_running.cpp's own
// generator comment says exactly this: "how quickly the related account
// trades after the client order is placed." An earlier version of this
// detector required the smaller order to precede the large one instead,
// which structurally could never fire against that generator's scenarios
// -- found and fixed via cpp/harness/'s Phase 10 evaluation; see
// cpp/harness/README.md.
//
// Reacts only to the Order arm's New status -- this is a statement about
// *order placement sequencing*, matching "sequencing" in the build guide's
// own phrasing, not about fills. A deterministic rule (score == 1.0 when
// it fires), not a heuristic like SpoofingLayeringDetector -- the
// combination of "related account" + "smaller size" + "shortly after" is
// already a conjunction of three independent conditions, each already
// binary/threshold-based, so a further continuous confidence score would
// just be re-deriving the same three yes/no facts without adding real
// information.
//
// Maintains a per (instrument, side) rolling window of recent New orders
// of ANY size (pruned to lookback_window_ns on every New) -- both the
// large orders being watched for a reaction and the smaller orders that
// might be reacting to one are recorded in the same window, since either
// role can be played by an order arriving at any point in the sequence.
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

    std::vector<Alert> handle_new(const tse::fix::Order& order, const AccountRegistry& accounts,
                                   int64_t book_snapshot_sequence);

    FrontRunningConfig config_;
    std::unordered_map<std::string, std::vector<RecentOrder>> recent_by_key_;  // key: instrument_id + "|" + side
};

}  // namespace tse::detectors
