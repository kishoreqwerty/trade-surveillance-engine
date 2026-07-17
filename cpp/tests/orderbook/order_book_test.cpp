#include "order_book.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

using tse::fix::Execution;
using tse::fix::Order;
using tse::fix::OrderStatus;
using tse::fix::OrderType;
using tse::fix::Side;
using tse::orderbook::DepthSnapshot;
using tse::orderbook::OrderBook;
using tse::orderbook::PriceLevel;
using tse::orderbook::RestingOrderSummary;

namespace {

constexpr const char* kInstrument = "ACME";

Order make_new_order(const std::string& order_id, Side side, double price, int64_t qty, int64_t timestamp_ns,
                      const std::string& account_id = "ACC-1") {
    Order order;
    order.order_id = order_id;
    order.orig_order_id = order_id;
    order.account_id = account_id;
    order.instrument_id = kInstrument;
    order.side = side;
    order.price = price;
    order.qty = qty;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = timestamp_ns;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    return order;
}

Order make_cancel(const std::string& new_order_id, const std::string& target_order_id, int64_t timestamp_ns) {
    Order order;
    order.order_id = new_order_id;
    order.orig_order_id = target_order_id;
    order.instrument_id = kInstrument;
    order.timestamp_ns = timestamp_ns;
    order.status = OrderStatus::kCancelled;
    return order;
}

Order make_replace(const std::string& new_order_id, const std::string& target_order_id, Side side, double price,
                    int64_t qty, int64_t timestamp_ns, const std::string& account_id = "ACC-1") {
    Order order;
    order.order_id = new_order_id;
    order.orig_order_id = target_order_id;
    order.account_id = account_id;
    order.instrument_id = kInstrument;
    order.side = side;
    order.price = price;
    order.qty = qty;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = timestamp_ns;
    order.status = OrderStatus::kReplaced;
    return order;
}

Execution make_execution(const std::string& order_id, Side side, double price, int64_t qty, int64_t timestamp_ns) {
    Execution execution;
    execution.trade_id = "EXE-" + order_id;
    execution.order_id = order_id;
    execution.account_id = "ACC-1";
    execution.instrument_id = kInstrument;
    execution.side = side;
    execution.price = price;
    execution.qty = qty;
    execution.timestamp_ns = timestamp_ns;
    execution.counterparty_account_id = "ACC-COUNTERPARTY";
    execution.venue = "SIM";
    return execution;
}

// Convenience: find a level by price within a side's vector (asserts it
// exists), so tests read as "the level at 100.00 should look like X"
// instead of relying on vector index/ordering for levels not under test.
const PriceLevel& find_level(const std::vector<PriceLevel>& levels, double price) {
    for (const auto& level : levels) {
        if (level.price == price) return level;
    }
    ADD_FAILURE() << "no price level found at " << price;
    static const PriceLevel empty{};
    return empty;
}

}  // namespace

// --- New order basics -------------------------------------------------

TEST(OrderBook, NewOrderRestsAtItsPriceLevel) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));

    EXPECT_EQ(book.qty_at_price(Side::kBuy, 100.00), 500);
    EXPECT_EQ(book.order_count_at_price(Side::kBuy, 100.00), 1u);
    ASSERT_TRUE(book.best_price(Side::kBuy).has_value());
    EXPECT_DOUBLE_EQ(*book.best_price(Side::kBuy), 100.00);
    EXPECT_FALSE(book.best_price(Side::kSell).has_value());
}

TEST(OrderBook, BestPriceIsHighestBidAndLowestAsk) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("B1", Side::kBuy, 99.50, 100, 1000));
    book.apply(make_new_order("B2", Side::kBuy, 100.00, 100, 1001));  // better bid
    book.apply(make_new_order("B3", Side::kBuy, 99.00, 100, 1002));
    book.apply(make_new_order("A1", Side::kSell, 101.00, 100, 1003));
    book.apply(make_new_order("A2", Side::kSell, 100.50, 100, 1004));  // better ask
    book.apply(make_new_order("A3", Side::kSell, 102.00, 100, 1005));

    ASSERT_TRUE(book.best_price(Side::kBuy).has_value());
    ASSERT_TRUE(book.best_price(Side::kSell).has_value());
    EXPECT_DOUBLE_EQ(*book.best_price(Side::kBuy), 100.00);
    EXPECT_DOUBLE_EQ(*book.best_price(Side::kSell), 100.50);
}

// No crossed-book invariant is enforced (see order_book.hpp's header
// comment for why): the book is a passive replica of externally-reported
// state, and upstream data could in principle be crossed. This proves that
// deliberate choice — a bid above the resting ask is accepted without
// throwing and both sides still report accurately, rather than the
// crossed condition being silently rejected, silently "fixed", or crashing.
TEST(OrderBook, AcceptsAndAccuratelyReflectsACrossedBookWithoutRejectingIt) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("A1", Side::kSell, 100.00, 200, 1000));
    EXPECT_NO_THROW(book.apply(make_new_order("B1", Side::kBuy, 101.00, 300, 1001)));  // crosses A1

    ASSERT_TRUE(book.best_price(Side::kBuy).has_value());
    ASSERT_TRUE(book.best_price(Side::kSell).has_value());
    EXPECT_DOUBLE_EQ(*book.best_price(Side::kBuy), 101.00);
    EXPECT_DOUBLE_EQ(*book.best_price(Side::kSell), 100.00);
    EXPECT_GT(*book.best_price(Side::kBuy), *book.best_price(Side::kSell));  // genuinely crossed, and that's fine
    EXPECT_EQ(book.qty_at_price(Side::kBuy, 101.00), 300);
    EXPECT_EQ(book.qty_at_price(Side::kSell, 100.00), 200);
}

TEST(OrderBook, DuplicateNewOrderIdThrows) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    EXPECT_THROW(book.apply(make_new_order("O1", Side::kBuy, 99.00, 200, 1001)), std::invalid_argument);
}

TEST(OrderBook, WrongInstrumentThrows) {
    OrderBook book(kInstrument);
    Order order = make_new_order("O1", Side::kBuy, 100.00, 500, 1000);
    order.instrument_id = "OTHER";
    EXPECT_THROW(book.apply(order), std::invalid_argument);
}

// --- Same price, same timestamp: FIFO is arrival order, not timestamp ---

TEST(OrderBook, SamePriceSameTimestampOrdersPreserveArrivalOrder) {
    OrderBook book(kInstrument);
    // Identical price AND identical timestamp_ns — only apply() call order
    // can break the tie. This is the case that would silently misbehave if
    // the book sorted resting orders by timestamp value instead of using
    // pure insertion order.
    book.apply(make_new_order("FIRST", Side::kBuy, 100.00, 300, 5000));
    book.apply(make_new_order("SECOND", Side::kBuy, 100.00, 400, 5000));
    book.apply(make_new_order("THIRD", Side::kBuy, 100.00, 500, 5000));

    DepthSnapshot snap = book.snapshot();
    const PriceLevel& level = find_level(snap.bids, 100.00);
    ASSERT_EQ(level.orders.size(), 3u);
    EXPECT_EQ(level.orders[0].order_id, "FIRST");
    EXPECT_EQ(level.orders[1].order_id, "SECOND");
    EXPECT_EQ(level.orders[2].order_id, "THIRD");
    EXPECT_EQ(level.total_qty, 300 + 400 + 500);
}

// --- Cancel -------------------------------------------------------------

TEST(OrderBook, CancelRemovesRestingOrder) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    book.apply(make_cancel("C1", "O1", 1001));

    EXPECT_EQ(book.qty_at_price(Side::kBuy, 100.00), 0);
    EXPECT_EQ(book.order_count_at_price(Side::kBuy, 100.00), 0u);
    EXPECT_FALSE(book.best_price(Side::kBuy).has_value());
}

TEST(OrderBook, CancelOfLastOrderAtLevelRemovesTheLevelItself) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    book.apply(make_new_order("O2", Side::kBuy, 99.00, 300, 1001));
    book.apply(make_cancel("C1", "O1", 1002));

    DepthSnapshot snap = book.snapshot();
    ASSERT_EQ(snap.bids.size(), 1u);
    EXPECT_DOUBLE_EQ(snap.bids[0].price, 99.00);
}

TEST(OrderBook, CancelOfUnknownOrderIsSilentNoOp) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    EXPECT_NO_THROW(book.apply(make_cancel("C1", "NEVER_EXISTED", 1001)));
    EXPECT_EQ(book.qty_at_price(Side::kBuy, 100.00), 500);  // untouched
}

TEST(OrderBook, DuplicateCancelIsSilentNoOp) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    book.apply(make_cancel("C1", "O1", 1001));
    EXPECT_NO_THROW(book.apply(make_cancel("C2", "O1", 1002)));
    EXPECT_FALSE(book.best_price(Side::kBuy).has_value());
}

TEST(OrderBook, CancelPreservesOtherOrdersAtSameLevel) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 300, 1000));
    book.apply(make_new_order("O2", Side::kBuy, 100.00, 400, 1001));
    book.apply(make_new_order("O3", Side::kBuy, 100.00, 500, 1002));
    book.apply(make_cancel("C1", "O2", 1003));  // cancel the middle one

    DepthSnapshot snap = book.snapshot();
    const PriceLevel& level = find_level(snap.bids, 100.00);
    ASSERT_EQ(level.orders.size(), 2u);
    EXPECT_EQ(level.orders[0].order_id, "O1");
    EXPECT_EQ(level.orders[1].order_id, "O3");
    EXPECT_EQ(level.total_qty, 300 + 500);
}

// --- Execution / partial fills ------------------------------------------

TEST(OrderBook, FullExecutionRemovesRestingOrder) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    book.apply(make_execution("O1", Side::kBuy, 100.00, 500, 1001));

    EXPECT_EQ(book.qty_at_price(Side::kBuy, 100.00), 0);
    EXPECT_FALSE(book.best_price(Side::kBuy).has_value());
}

// Direct snapshot()-level check that a fully-executed level disappears
// entirely rather than lingering as a zero-qty entry — CancelOfLastOrder-
// AtLevelRemovesTheLevelItself proves this for the Cancel path; this is the
// same proof for the Execution path, since both route through the same
// remove_resting() but weren't both checked against snapshot() directly.
TEST(OrderBook, FullExecutionOfLastOrderAtLevelRemovesTheLevelFromSnapshot) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    book.apply(make_new_order("O2", Side::kBuy, 99.00, 300, 1001));
    book.apply(make_execution("O1", Side::kBuy, 100.00, 500, 1002));  // fully fills O1

    DepthSnapshot snap = book.snapshot();
    ASSERT_EQ(snap.bids.size(), 1u);  // not 2 with a stray zero-qty 100.00 entry
    EXPECT_DOUBLE_EQ(snap.bids[0].price, 99.00);
}

TEST(OrderBook, PartialFillReducesQtyAndRetainsQueuePosition) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    book.apply(make_new_order("O2", Side::kBuy, 100.00, 300, 1001));
    book.apply(make_execution("O1", Side::kBuy, 100.00, 200, 1002));  // partial fill of O1

    DepthSnapshot snap = book.snapshot();
    const PriceLevel& level = find_level(snap.bids, 100.00);
    ASSERT_EQ(level.orders.size(), 2u);
    // O1 stays first (priority retained) but with reduced qty.
    EXPECT_EQ(level.orders[0].order_id, "O1");
    EXPECT_EQ(level.orders[0].qty, 300);  // 500 - 200
    EXPECT_EQ(level.orders[1].order_id, "O2");
    EXPECT_EQ(level.orders[1].qty, 300);
    EXPECT_EQ(level.total_qty, 600);
}

TEST(OrderBook, SeveralPartialFillsAccumulateCorrectly) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 1000, 1000));
    book.apply(make_execution("O1", Side::kBuy, 100.00, 300, 1001));
    book.apply(make_execution("O1", Side::kBuy, 100.00, 200, 1002));
    book.apply(make_execution("O1", Side::kBuy, 100.00, 500, 1003));  // exactly exhausts it

    EXPECT_EQ(book.qty_at_price(Side::kBuy, 100.00), 0);
    EXPECT_FALSE(book.best_price(Side::kBuy).has_value());
}

TEST(OrderBook, CancelAfterPartialFillCancelsExactRemainder) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    book.apply(make_execution("O1", Side::kBuy, 100.00, 200, 1001));  // remaining: 300
    book.apply(make_cancel("C1", "O1", 1002));

    EXPECT_EQ(book.qty_at_price(Side::kBuy, 100.00), 0);
    EXPECT_EQ(book.order_count_at_price(Side::kBuy, 100.00), 0u);
}

TEST(OrderBook, ExecutionExceedingRestingQtyThrows) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    EXPECT_THROW(book.apply(make_execution("O1", Side::kBuy, 100.00, 501, 1001)), std::invalid_argument);
}

TEST(OrderBook, ExecutionAgainstUnknownOrderThrows) {
    OrderBook book(kInstrument);
    EXPECT_THROW(book.apply(make_execution("NEVER_EXISTED", Side::kBuy, 100.00, 100, 1000)),
                 std::invalid_argument);
}

// --- Replace: priority retention (qty decrease at same price) ----------

TEST(OrderBook, ReplaceQtyDecreaseAtSamePriceRetainsQueuePosition) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    book.apply(make_new_order("O2", Side::kBuy, 100.00, 300, 1001));
    // Replace O1: same price, smaller qty. Should stay ahead of O2.
    book.apply(make_replace("R1", "O1", Side::kBuy, 100.00, 200, 1002));

    DepthSnapshot snap = book.snapshot();
    const PriceLevel& level = find_level(snap.bids, 100.00);
    ASSERT_EQ(level.orders.size(), 2u);
    EXPECT_EQ(level.orders[0].order_id, "R1");  // new id, but still first in queue
    EXPECT_EQ(level.orders[0].qty, 200);
    EXPECT_EQ(level.orders[1].order_id, "O2");
}

TEST(OrderBook, ReplaceWithUnchangedQtyAndPriceRetainsQueuePosition) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    book.apply(make_new_order("O2", Side::kBuy, 100.00, 300, 1001));
    book.apply(make_replace("R1", "O1", Side::kBuy, 100.00, 500, 1002));  // no actual change

    DepthSnapshot snap = book.snapshot();
    const PriceLevel& level = find_level(snap.bids, 100.00);
    ASSERT_EQ(level.orders.size(), 2u);
    EXPECT_EQ(level.orders[0].order_id, "R1");
    EXPECT_EQ(level.orders[1].order_id, "O2");
}

// --- Replace: priority loss (qty increase or price change) -------------

TEST(OrderBook, ReplaceQtyIncreaseAtSamePriceLosesQueuePosition) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    book.apply(make_new_order("O2", Side::kBuy, 100.00, 300, 1001));
    // Replace O1 with a LARGER qty at the same price -> loses priority, moves behind O2.
    book.apply(make_replace("R1", "O1", Side::kBuy, 100.00, 600, 1002));

    DepthSnapshot snap = book.snapshot();
    const PriceLevel& level = find_level(snap.bids, 100.00);
    ASSERT_EQ(level.orders.size(), 2u);
    EXPECT_EQ(level.orders[0].order_id, "O2");
    EXPECT_EQ(level.orders[1].order_id, "R1");
    EXPECT_EQ(level.orders[1].qty, 600);
}

TEST(OrderBook, ReplacePriceChangeLosesQueuePositionEvenWithQtyDecrease) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    book.apply(make_new_order("O2", Side::kBuy, 100.00, 300, 1001));
    // Replace O1 with a smaller qty but a DIFFERENT price -> still loses priority.
    book.apply(make_replace("R1", "O1", Side::kBuy, 99.50, 100, 1002));

    DepthSnapshot snap = book.snapshot();
    // Old price level (100.00) now has only O2.
    const PriceLevel& old_level = find_level(snap.bids, 100.00);
    ASSERT_EQ(old_level.orders.size(), 1u);
    EXPECT_EQ(old_level.orders[0].order_id, "O2");
    // New price level (99.50) has R1.
    const PriceLevel& new_level = find_level(snap.bids, 99.50);
    ASSERT_EQ(new_level.orders.size(), 1u);
    EXPECT_EQ(new_level.orders[0].order_id, "R1");
    EXPECT_EQ(new_level.orders[0].qty, 100);
}

TEST(OrderBook, ReplaceMovingToPriceLevelWithExistingOrdersJoinsAtBack) {
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));  // to be replaced away
    book.apply(make_new_order("O2", Side::kBuy, 99.00, 200, 1001));
    book.apply(make_new_order("O3", Side::kBuy, 99.00, 300, 1002));
    book.apply(make_replace("R1", "O1", Side::kBuy, 99.00, 100, 1003));  // moves into the 99.00 level

    DepthSnapshot snap = book.snapshot();
    const PriceLevel& level = find_level(snap.bids, 99.00);
    ASSERT_EQ(level.orders.size(), 3u);
    EXPECT_EQ(level.orders[0].order_id, "O2");
    EXPECT_EQ(level.orders[1].order_id, "O3");
    EXPECT_EQ(level.orders[2].order_id, "R1");  // joins at the back, not the front
}

TEST(OrderBook, ReplaceReferencingUnknownOrderThrows) {
    OrderBook book(kInstrument);
    EXPECT_THROW(book.apply(make_replace("R1", "NEVER_EXISTED", Side::kBuy, 100.00, 100, 1000)),
                 std::invalid_argument);
}

TEST(OrderBook, ReplaceThenPartialFillThenCancelWorksThroughIdentityChange) {
    // Exercises identity continuity: after a priority-retaining replace,
    // the order lives on under the *new* order_id — executions and a later
    // cancel must reference that new id, not the original one.
    OrderBook book(kInstrument);
    book.apply(make_new_order("O1", Side::kBuy, 100.00, 500, 1000));
    book.apply(make_replace("R1", "O1", Side::kBuy, 100.00, 400, 1001));  // qty decrease, retains priority
    book.apply(make_execution("R1", Side::kBuy, 100.00, 150, 1002));      // partial fill against the new id
    EXPECT_EQ(book.qty_at_price(Side::kBuy, 100.00), 250);

    book.apply(make_cancel("C1", "R1", 1003));
    EXPECT_EQ(book.qty_at_price(Side::kBuy, 100.00), 0);

    // The original id is fully retired — nothing should still reference it.
    EXPECT_THROW(book.apply(make_execution("O1", Side::kBuy, 100.00, 1, 1004)), std::invalid_argument);
}
