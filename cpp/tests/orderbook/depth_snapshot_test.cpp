#include "depth_snapshot.hpp"
#include "order_book.hpp"

#include <gtest/gtest.h>

#include <variant>
#include <vector>

using tse::fix::Execution;
using tse::fix::Order;
using tse::fix::OrderStatus;
using tse::fix::OrderType;
using tse::fix::Side;
using tse::orderbook::DepthSnapshot;
using tse::orderbook::OrderBook;

namespace {

constexpr const char* kInstrument = "ACME";

using Event = std::variant<Order, Execution>;

Order new_order(const std::string& id, Side side, double price, int64_t qty, int64_t ts,
                 const std::string& account = "ACC-1") {
    Order order;
    order.order_id = id;
    order.orig_order_id = id;
    order.account_id = account;
    order.instrument_id = kInstrument;
    order.side = side;
    order.price = price;
    order.qty = qty;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = ts;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    return order;
}

Order cancel(const std::string& new_id, const std::string& target, int64_t ts) {
    Order order;
    order.order_id = new_id;
    order.orig_order_id = target;
    order.instrument_id = kInstrument;
    order.timestamp_ns = ts;
    order.status = OrderStatus::kCancelled;
    return order;
}

Order replace(const std::string& new_id, const std::string& target, Side side, double price, int64_t qty,
              int64_t ts) {
    Order order;
    order.order_id = new_id;
    order.orig_order_id = target;
    order.account_id = "ACC-1";
    order.instrument_id = kInstrument;
    order.side = side;
    order.price = price;
    order.qty = qty;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = ts;
    order.status = OrderStatus::kReplaced;
    return order;
}

Execution fill(const std::string& order_id, Side side, double price, int64_t qty, int64_t ts) {
    Execution execution;
    execution.trade_id = "EXE-" + order_id + "-" + std::to_string(ts);
    execution.order_id = order_id;
    execution.account_id = "ACC-1";
    execution.instrument_id = kInstrument;
    execution.side = side;
    execution.price = price;
    execution.qty = qty;
    execution.timestamp_ns = ts;
    execution.counterparty_account_id = "ACC-COUNTERPARTY";
    execution.venue = "SIM";
    return execution;
}

void apply_event(OrderBook& book, const Event& event) {
    std::visit([&book](const auto& concrete) { book.apply(concrete); }, event);
}

// A realistic mixed sequence: several resting orders, partial fills,
// a cancel, and both flavors of replace (priority-losing and
// priority-retaining) interleaved across two price levels.
std::vector<Event> representative_sequence() {
    return {
        Event{new_order("O1", Side::kBuy, 100.00, 500, 1000)},
        Event{new_order("O2", Side::kBuy, 100.00, 300, 1001)},
        Event{new_order("O3", Side::kBuy, 99.50, 200, 1002)},
        Event{fill("O1", Side::kBuy, 100.00, 150, 1003)},                   // partial fill
        Event{replace("R1", "O2", Side::kBuy, 100.00, 250, 1004)},          // qty decrease, retains priority
        Event{new_order("O4", Side::kSell, 101.00, 400, 1005)},
        Event{replace("R2", "O3", Side::kBuy, 100.50, 200, 1006)},          // price change, loses priority
        Event{cancel("C1", "O1", 1007)},
        Event{fill("O4", Side::kSell, 101.00, 100, 1008)},                  // partial fill on ask side
        Event{new_order("O5", Side::kBuy, 100.50, 100, 1009)},              // joins R2's level
    };
}

}  // namespace

// The core "provably consistent" claim: a snapshot taken after N events from
// a live book must be byte-for-byte (via DepthSnapshot::operator==, which
// compares every field including sequence/timestamp) identical to the
// snapshot of an entirely fresh OrderBook fed only the same first N events.
// If snapshot() ever captured stale internal state, missed an update, or
// double-applied one, this is what would catch it — not just "the numbers
// look plausible for this one hand-checked point."
TEST(DepthSnapshotConsistency, SnapshotAtEveryPrefixMatchesFreshReplay) {
    std::vector<Event> events = representative_sequence();

    OrderBook live_book(kInstrument);
    for (std::size_t n = 1; n <= events.size(); ++n) {
        apply_event(live_book, events[n - 1]);
        DepthSnapshot live_snapshot = live_book.snapshot();

        OrderBook fresh_book(kInstrument);
        for (std::size_t i = 0; i < n; ++i) {
            apply_event(fresh_book, events[i]);
        }
        DepthSnapshot fresh_snapshot = fresh_book.snapshot();

        EXPECT_EQ(live_snapshot, fresh_snapshot) << "diverged after " << n << " events";
        EXPECT_EQ(live_snapshot.sequence, n);
    }
}

// Same property, restated without relying on operator== working correctly
// end-to-end: check the specific fields a spoofing/evidence consumer would
// actually read, by hand, at one well-understood midpoint (after the first
// 5 events of representative_sequence()).
TEST(DepthSnapshotConsistency, HandVerifiedMidpointSnapshot) {
    std::vector<Event> events = representative_sequence();
    OrderBook book(kInstrument);
    for (std::size_t i = 0; i < 5; ++i) apply_event(book, events[i]);  // through the R1 replace

    DepthSnapshot snap = book.snapshot();
    EXPECT_EQ(snap.sequence, 5u);
    EXPECT_EQ(snap.last_event_timestamp_ns, 1004);

    // Bids: level 100.00 has O1 (500-150=350) then R1 (250, replaced from
    // O2's 300 with a qty decrease so it kept its queue slot). Level 99.50
    // has O3 (200), untouched so far.
    ASSERT_EQ(snap.bids.size(), 2u);
    EXPECT_DOUBLE_EQ(snap.bids[0].price, 100.00);  // best bid first
    ASSERT_EQ(snap.bids[0].orders.size(), 2u);
    EXPECT_EQ(snap.bids[0].orders[0].order_id, "O1");
    EXPECT_EQ(snap.bids[0].orders[0].qty, 350);
    EXPECT_EQ(snap.bids[0].orders[1].order_id, "R1");
    EXPECT_EQ(snap.bids[0].orders[1].qty, 250);
    EXPECT_EQ(snap.bids[0].total_qty, 600);

    EXPECT_DOUBLE_EQ(snap.bids[1].price, 99.50);
    ASSERT_EQ(snap.bids[1].orders.size(), 1u);
    EXPECT_EQ(snap.bids[1].orders[0].order_id, "O3");
    EXPECT_EQ(snap.bids[1].orders[0].qty, 200);

    EXPECT_TRUE(snap.asks.empty());
}

// And at the very end of the sequence, to hand-verify the priority-losing
// replace (R2) and the cancel/partial-fill interactions all landed
// correctly together, not just individually.
TEST(DepthSnapshotConsistency, HandVerifiedFinalSnapshot) {
    std::vector<Event> events = representative_sequence();
    OrderBook book(kInstrument);
    for (const Event& event : events) apply_event(book, event);

    DepthSnapshot snap = book.snapshot();
    EXPECT_EQ(snap.sequence, events.size());
    EXPECT_EQ(snap.last_event_timestamp_ns, 1009);

    // Bids: O1 was cancelled entirely. 100.00 now holds only R1 (250).
    // 100.50 holds R2 (200, moved in via price-change replace) then O5
    // (100, joined after) — R2 first since it arrived at that level first.
    // 99.50 is gone (O3 left it via replace, nothing else was ever there).
    ASSERT_EQ(snap.bids.size(), 2u);
    EXPECT_DOUBLE_EQ(snap.bids[0].price, 100.50);  // best bid
    ASSERT_EQ(snap.bids[0].orders.size(), 2u);
    EXPECT_EQ(snap.bids[0].orders[0].order_id, "R2");
    EXPECT_EQ(snap.bids[0].orders[0].qty, 200);
    EXPECT_EQ(snap.bids[0].orders[1].order_id, "O5");
    EXPECT_EQ(snap.bids[0].orders[1].qty, 100);

    EXPECT_DOUBLE_EQ(snap.bids[1].price, 100.00);
    ASSERT_EQ(snap.bids[1].orders.size(), 1u);
    EXPECT_EQ(snap.bids[1].orders[0].order_id, "R1");
    EXPECT_EQ(snap.bids[1].orders[0].qty, 250);

    // Asks: O4 partially filled (400-100=300), still resting.
    ASSERT_EQ(snap.asks.size(), 1u);
    EXPECT_DOUBLE_EQ(snap.asks[0].price, 101.00);
    ASSERT_EQ(snap.asks[0].orders.size(), 1u);
    EXPECT_EQ(snap.asks[0].orders[0].order_id, "O4");
    EXPECT_EQ(snap.asks[0].orders[0].qty, 300);
}

// snapshot() must be a pure read — calling it repeatedly, or interleaved
// with further apply() calls, must never itself mutate book state.
TEST(DepthSnapshotConsistency, SnapshotIsReadOnlyAndRepeatable) {
    OrderBook book(kInstrument);
    book.apply(new_order("O1", Side::kBuy, 100.00, 500, 1000));

    DepthSnapshot first = book.snapshot();
    DepthSnapshot second = book.snapshot();
    EXPECT_EQ(first, second);

    book.apply(new_order("O2", Side::kBuy, 99.00, 100, 1001));
    DepthSnapshot third = book.snapshot();
    EXPECT_NE(first, third);  // book genuinely changed
    EXPECT_EQ(first.sequence, 1u);
    EXPECT_EQ(third.sequence, 2u);
}
