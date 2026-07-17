#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

#include "depth_snapshot.hpp"
#include "types.hpp"  // tse::fix::Order, tse::fix::Execution, tse::fix::Side

namespace tse::orderbook {

// Hand-rolled, single-instrument, price-time-priority order book. Applies
// Order (New/Cancel/Replace) and Execution events exactly as reported —
// this class is a passive *replica* of an exchange's book reconstructed
// from FIX ExecutionReport/NewOrderSingle/OrderCancel(Replace) flow, not a
// matching engine: it never itself decides who trades with whom (that
// decision already happened at the exchange and arrives here as an
// Execution naming the exact resting order_id it filled). See
// P2_trade_surveillance_engine_architecture.md §3 ("OrderBook") and §4
// (data flow) for the surrounding pipeline.
//
// Price-time priority: "time" here means *processing order* (the order in
// which apply() is called), not the numeric value of any Order's
// timestamp_ns field. Each price level's resting orders are kept in a
// std::list in strict push_back/erase order — never re-sorted by
// timestamp — so two orders that happen to carry identical timestamp_ns
// (a real possibility; FIX timestamps aren't infinitely fine-grained) are
// still correctly tie-broken by which one was actually applied first. This
// is deliberate, not an oversight: see
// cpp/tests/orderbook/order_book_test.cpp's
// SamePriceSameTimestampOrdersPreserveArrivalOrder.
//
// Single-threaded by design for this phase (see CLAUDE.md's phase
// discipline: orderbook/ must be proven correct in isolation before Phase 6
// wires it into the concurrent live pipeline) — no internal synchronization.
//
// Single-instrument by design: one OrderBook instance holds one
// instrument's state (architecture doc §2: "per-instrument state"), and
// apply() throws if handed an event for a different instrument_id — a
// caller-side bug, not something this class silently tolerates. Routing
// events to the right per-instrument OrderBook (a registry/map keyed by
// instrument_id) is explicitly out of scope for this phase: it's Phase 6's
// live-pipeline wiring concern, not the book engine's, and building it here
// would be getting ahead of the phase that actually needs it.
//
// No crossed-book invariant is enforced (best bid >= best ask is never
// checked or rejected). This is deliberate, not an oversight: this class
// is a passive replica of externally-reported state, not a matching
// engine, and upstream data (out-of-order FIX delivery, exchange-side
// issues, or — relevant to this project's actual purpose — the very market
// abuse this system exists to detect) can in principle produce a
// momentarily crossed book. A hand-rolled invariant that threw on best_bid
// >= best_ask would make the surveillance system stop observing exactly
// when something anomalous is happening upstream, which defeats the
// point. If a caller wants to detect/flag a crossed state, that's a
// Phase 5 detector's job, reading through best_price() — not this class's.
class OrderBook {
public:
    explicit OrderBook(std::string instrument_id);

    // Dispatches on order.status:
    //   kNew      — inserts at the back of its price level's FIFO queue.
    //   kCancelled — removes the resting order named by order.orig_order_id.
    //                A reference to an order that's no longer resting (already
    //                fully filled, already cancelled, or a duplicate cancel —
    //                all ordinary races in real order flow, not corruption) is
    //                a silent no-op, not an error.
    //   kReplaced  — amends the resting order named by order.orig_order_id to
    //                this Order's price/qty, under standard price-time-priority
    //                amend rules: a price change or a quantity *increase* loses
    //                time priority (removed, then re-inserted at the back of
    //                the — possibly new — price level's queue, with this
    //                message's timestamp_ns); a quantity decrease (or no
    //                change) at the same price *retains* priority (mutated in
    //                place, same queue position). order.order_id always becomes
    //                the resting order's new identity either way — a Replace
    //                mints a new ClOrdID even when priority is retained (real
    //                FIX semantics; only queue *position* is what "priority"
    //                governs). Replace referencing an order_id that isn't
    //                currently resting throws std::invalid_argument — unlike
    //                Cancel, an amend to something not there indicates upstream
    //                inconsistency, not a normal race.
    //
    // Throws std::invalid_argument if order.instrument_id doesn't match this
    // book's instrument, or (New only) if order.order_id already denotes a
    // currently-resting order.
    void apply(const tse::fix::Order& order);

    // Reduces the resting order named by execution.order_id by
    // execution.qty. Reaching zero removes it from the book entirely
    // (equivalent to a Cancel, from the book's point of view); a nonzero
    // remainder stays at its exact existing queue position — a partial
    // fill never causes loss of time priority.
    //
    // Throws std::invalid_argument if execution.instrument_id doesn't match
    // this book's instrument, if execution.order_id doesn't name a
    // currently-resting order, or if execution.qty exceeds that order's
    // remaining resting qty (an overfill — a genuine invariant violation
    // for this single-threaded, strictly-ordered-application model, not an
    // expected race).
    void apply(const tse::fix::Execution& execution);

    // Full per-side, per-price-level, per-order state — see
    // depth_snapshot.hpp for why `sequence` makes this checkable against
    // the exact update sequence that produced it, not just "looks right."
    DepthSnapshot snapshot() const;

    // Read-only depth-at-level queries (architecture doc §3), independent
    // of snapshot() — useful where a caller only needs one level/side and
    // building a full snapshot would be wasted work.
    int64_t qty_at_price(tse::fix::Side side, double price) const;
    std::size_t order_count_at_price(tse::fix::Side side, double price) const;
    std::optional<double> best_price(tse::fix::Side side) const;

    const std::string& instrument_id() const { return instrument_id_; }
    uint64_t sequence() const { return sequence_; }

private:
    struct RestingOrder {
        std::string order_id;
        std::string account_id;
        tse::fix::Side side{tse::fix::Side::kBuy};
        int64_t qty{0};
        int64_t timestamp_ns{0};
    };

    using OrderQueue = std::list<RestingOrder>;
    // Ascending by price for both sides — best-first iteration is achieved
    // by direction (asks: forward from begin(); bids: reverse from
    // rbegin()) rather than by using a different comparator per side. This
    // keeps bids_ and asks_ the exact same type, avoiding a templated- or
    // duplicated-comparator split between otherwise-identical code paths.
    using PriceLevels = std::map<double, OrderQueue>;

    struct OrderIndexEntry {
        tse::fix::Side side;
        double price;
        OrderQueue::iterator it;
    };

    PriceLevels& levels_for(tse::fix::Side side);
    const PriceLevels& levels_for(tse::fix::Side side) const;

    // Shared by New and by a priority-losing Replace's re-insertion step.
    void insert_new(const tse::fix::Order& order);

    // Erases the resting order the given order_index_ iterator points at:
    // from its price level's queue, from the price-level map if that
    // empties the level, and from order_index_ itself.
    void remove_resting(std::unordered_map<std::string, OrderIndexEntry>::iterator index_it);

    void cancel(const std::string& target_order_id);
    void replace(const tse::fix::Order& order);

    PriceLevel level_summary(double price, const OrderQueue& queue) const;

    std::string instrument_id_;
    PriceLevels bids_;
    PriceLevels asks_;
    std::unordered_map<std::string, OrderIndexEntry> order_index_;
    uint64_t sequence_{0};
    int64_t last_event_timestamp_ns_{0};
};

}  // namespace tse::orderbook
