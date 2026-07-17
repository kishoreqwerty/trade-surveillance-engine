#pragma once

#include <string>
#include <vector>

#include "account_registry.hpp"
#include "alert.hpp"
#include "detector_event.hpp"
#include "order_book.hpp"

namespace tse::detectors {

// Common interface every detector implements, so Phase 10's evaluation
// harness can swap/compare them uniformly (architecture doc §3).
//
// Deliberate deviation from the architecture doc's original snippet, which
// declared `evaluate(const OrderBook&, const Order&, const
// AccountRegistry&)`. Implementing the five detectors this phase actually
// specifies surfaced a real gap, not a style preference: WashTradeDetector
// and MarkingTheCloseDetector are both fundamentally about *executions*
// (who traded with whom, what actually printed) — a wash trade is a
// completed cross between related accounts, not an inference from resting
// order state, and "concentrated activity near the close" means executed
// volume, not merely orders sitting in the book. Neither can be
// implemented correctly against Order-only input. `DetectorEvent` (Order or
// Execution — see detector_event.hpp) is the minimal fix: every detector
// still receives the post-update OrderBook and AccountRegistry unchanged,
// and detectors that only need Order data (SpoofingLayering,
// FrontRunning, StatisticalBaseline) simply never match the Execution arm.
// P2_trade_surveillance_engine_architecture.md §3 has been updated to
// match.
//
// No exceptions cross this boundary (CLAUDE.md's style rule) — an
// implementation returns an empty vector for "nothing fired," never
// throws. evaluate() is intentionally non-const: every detector in this
// phase is a stateful streaming evaluator (spoofing needs order lifecycle
// history, marking-the-close needs a running closing-window volume tally,
// etc.), called once per incoming event to update that state and check
// its pattern against the result — not a pure function of its arguments.
class IDetector {
public:
    virtual ~IDetector() = default;

    virtual std::vector<Alert> evaluate(const tse::orderbook::OrderBook& book, const DetectorEvent& incoming,
                                         const AccountRegistry& accounts) = 0;

    virtual std::string name() const = 0;
};

}  // namespace tse::detectors
