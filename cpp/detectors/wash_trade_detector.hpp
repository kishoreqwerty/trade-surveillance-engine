#pragma once

#include "i_detector.hpp"

namespace tse::detectors {

// A deterministic rule, not a heuristic: fires whenever an Execution
// crosses two related accounts (same beneficial owner, an explicit link,
// or literally the same account trading with itself — see
// AccountRegistry::is_related). Wash trading is fundamentally a statement
// about a *completed trade*, not resting order state, so this only ever
// reacts to the Execution arm of DetectorEvent; New/Cancel/Replace Order
// events are ignored. Always emits score == 1.0 when it fires — there's no
// partial-confidence notion for "did these two related accounts actually
// trade with each other," unlike the heuristic detectors.
class WashTradeDetector : public IDetector {
public:
    std::vector<Alert> evaluate(const tse::orderbook::OrderBook& book, const DetectorEvent& incoming,
                                 const AccountRegistry& accounts) override;

    std::string name() const override { return "WashTradeDetector"; }
};

}  // namespace tse::detectors
