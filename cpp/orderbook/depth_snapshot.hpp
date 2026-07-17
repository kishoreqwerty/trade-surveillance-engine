#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tse::orderbook {

// A single resting order's public footprint within a price level, in FIFO
// priority order — orders[0] within a PriceLevel has the highest time
// priority at that price. Deliberately doesn't expose an arrival timestamp:
// priority here is entirely a function of queue *position*, not of any
// timestamp field's value (see OrderBook's header comment on same-price/
// same-timestamp orders), so a timestamp field would invite callers to
// re-derive priority by sorting on it instead of trusting position — a
// source of subtle bugs, not a useful convenience.
struct RestingOrderSummary {
    std::string order_id;
    std::string account_id;
    int64_t qty{0};  // remaining resting quantity
};

// One side's state at a single price. `total_qty` is a denormalized sum of
// `orders[*].qty` — kept as its own field (not computed on every read)
// because depth-at-a-glance is the primary reason DepthSnapshot exists
// (spoofing detector's "% of visible depth" check, dashboard depth
// visualization), so summing on every access would make the common case pay
// for the uncommon one (per-order detail, needed far less often).
struct PriceLevel {
    double price{0.0};
    int64_t total_qty{0};
    std::vector<RestingOrderSummary> orders;  // FIFO priority order
};

// A full book snapshot for one instrument. `sequence` is a running count of
// apply() calls the producing OrderBook has processed — not a display
// value, but what makes "provably consistent with the update sequence that
// produced it" checkable: a snapshot taken after N events must equal, field
// for field, the snapshot of a *fresh* OrderBook fed only the same first N
// events (see cpp/tests/orderbook/depth_snapshot_test.cpp). Two independent
// replays converging on identical (sequence, last_event_timestamp_ns,
// bids, asks) is the proof; nothing here is trusted "by construction."
struct DepthSnapshot {
    std::string instrument_id;
    uint64_t sequence{0};
    int64_t last_event_timestamp_ns{0};
    std::vector<PriceLevel> bids;  // best (highest price) first
    std::vector<PriceLevel> asks;  // best (lowest price) first
};

inline bool operator==(const RestingOrderSummary& a, const RestingOrderSummary& b) {
    return a.order_id == b.order_id && a.account_id == b.account_id && a.qty == b.qty;
}
inline bool operator!=(const RestingOrderSummary& a, const RestingOrderSummary& b) { return !(a == b); }

inline bool operator==(const PriceLevel& a, const PriceLevel& b) {
    return a.price == b.price && a.total_qty == b.total_qty && a.orders == b.orders;
}
inline bool operator!=(const PriceLevel& a, const PriceLevel& b) { return !(a == b); }

inline bool operator==(const DepthSnapshot& a, const DepthSnapshot& b) {
    return a.instrument_id == b.instrument_id && a.sequence == b.sequence &&
           a.last_event_timestamp_ns == b.last_event_timestamp_ns && a.bids == b.bids && a.asks == b.asks;
}
inline bool operator!=(const DepthSnapshot& a, const DepthSnapshot& b) { return !(a == b); }

}  // namespace tse::orderbook
