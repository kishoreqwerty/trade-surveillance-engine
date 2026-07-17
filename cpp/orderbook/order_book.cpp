#include "order_book.hpp"

#include <stdexcept>
#include <utility>

namespace tse::orderbook {

using tse::fix::Execution;
using tse::fix::Order;
using tse::fix::OrderStatus;
using tse::fix::Side;

OrderBook::OrderBook(std::string instrument_id) : instrument_id_(std::move(instrument_id)) {}

OrderBook::PriceLevels& OrderBook::levels_for(Side side) { return side == Side::kBuy ? bids_ : asks_; }

const OrderBook::PriceLevels& OrderBook::levels_for(Side side) const {
    return side == Side::kBuy ? bids_ : asks_;
}

void OrderBook::insert_new(const Order& order) {
    if (order_index_.count(order.order_id) != 0) {
        throw std::invalid_argument("OrderBook::apply: duplicate order_id on New: " + order.order_id);
    }
    PriceLevels& levels = levels_for(order.side);
    OrderQueue& queue = levels[order.price];  // creates an empty level if this is the first order at this price
    queue.push_back(RestingOrder{order.order_id, order.account_id, order.side, order.qty, order.timestamp_ns});
    order_index_[order.order_id] = OrderIndexEntry{order.side, order.price, std::prev(queue.end())};
}

void OrderBook::remove_resting(std::unordered_map<std::string, OrderIndexEntry>::iterator index_it) {
    const OrderIndexEntry entry = index_it->second;  // copy: index_it is erased before the level lookup below
    PriceLevels& levels = levels_for(entry.side);
    auto level_it = levels.find(entry.price);
    level_it->second.erase(entry.it);
    if (level_it->second.empty()) {
        levels.erase(level_it);
    }
    order_index_.erase(index_it);
}

void OrderBook::cancel(const std::string& target_order_id) {
    auto found = order_index_.find(target_order_id);
    if (found == order_index_.end()) {
        // Not an error: a Cancel can legitimately race a fill or an earlier
        // Cancel for the same order in real order flow. See apply(Order)'s
        // header comment.
        return;
    }
    remove_resting(found);
}

void OrderBook::replace(const Order& order) {
    auto found = order_index_.find(order.orig_order_id);
    if (found == order_index_.end()) {
        throw std::invalid_argument("OrderBook::apply: Replace references an order that isn't resting: " +
                                     order.orig_order_id);
    }

    const OrderIndexEntry old_entry = found->second;
    RestingOrder& resting = *old_entry.it;

    const bool price_changed = order.price != old_entry.price;
    const bool qty_increased = order.qty > resting.qty;

    if (price_changed || qty_increased) {
        // Priority-losing amend: leaves its current queue position (and
        // possibly price level) entirely and re-enters as a brand-new
        // resting order at the back of the (possibly new) level's queue,
        // carrying this message's own timestamp_ns.
        remove_resting(found);
        Order new_resting = order;
        new_resting.status = OrderStatus::kNew;  // reuse insert_new's invariants (duplicate-id check, indexing)
        insert_new(new_resting);
    } else {
        // Priority-retaining amend: quantity decreased (or is unchanged) at
        // the same price. Mutated in place — same queue position, same
        // original time priority — but the resting order's identity still
        // moves to this request's order_id, since a Replace always mints a
        // new ClOrdID even when priority is retained.
        resting.qty = order.qty;
        resting.order_id = order.order_id;
        OrderIndexEntry updated_entry = old_entry;
        order_index_.erase(found);
        order_index_[order.order_id] = updated_entry;
    }
}

void OrderBook::apply(const Order& order) {
    if (order.instrument_id != instrument_id_) {
        throw std::invalid_argument("OrderBook::apply(Order): instrument_id mismatch for book '" + instrument_id_ +
                                     "': got '" + order.instrument_id + "'");
    }
    ++sequence_;
    last_event_timestamp_ns_ = order.timestamp_ns;

    switch (order.status) {
        case OrderStatus::kNew:
            insert_new(order);
            break;
        case OrderStatus::kCancelled:
            cancel(order.orig_order_id);
            break;
        case OrderStatus::kReplaced:
            replace(order);
            break;
    }
}

void OrderBook::apply(const Execution& execution) {
    if (execution.instrument_id != instrument_id_) {
        throw std::invalid_argument("OrderBook::apply(Execution): instrument_id mismatch for book '" +
                                     instrument_id_ + "': got '" + execution.instrument_id + "'");
    }
    ++sequence_;
    last_event_timestamp_ns_ = execution.timestamp_ns;

    auto found = order_index_.find(execution.order_id);
    if (found == order_index_.end()) {
        throw std::invalid_argument("OrderBook::apply: Execution references an order that isn't resting: " +
                                     execution.order_id);
    }
    RestingOrder& resting = *found->second.it;
    if (execution.qty > resting.qty) {
        throw std::invalid_argument("OrderBook::apply: Execution qty (" + std::to_string(execution.qty) +
                                     ") exceeds resting qty (" + std::to_string(resting.qty) +
                                     ") for order_id: " + execution.order_id);
    }
    resting.qty -= execution.qty;
    if (resting.qty == 0) {
        remove_resting(found);  // fully filled — leaves the book, same as a cancel
    }
    // else: partial fill. Left in place at its exact existing queue
    // position — only qty changes, priority doesn't.
}

PriceLevel OrderBook::level_summary(double price, const OrderQueue& queue) const {
    PriceLevel level;
    level.price = price;
    level.orders.reserve(queue.size());
    int64_t total = 0;
    for (const RestingOrder& resting : queue) {
        level.orders.push_back(RestingOrderSummary{resting.order_id, resting.account_id, resting.qty});
        total += resting.qty;
    }
    level.total_qty = total;
    return level;
}

DepthSnapshot OrderBook::snapshot() const {
    DepthSnapshot snap;
    snap.instrument_id = instrument_id_;
    snap.sequence = sequence_;
    snap.last_event_timestamp_ns = last_event_timestamp_ns_;

    snap.bids.reserve(bids_.size());
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
        snap.bids.push_back(level_summary(it->first, it->second));
    }
    snap.asks.reserve(asks_.size());
    for (const auto& [price, queue] : asks_) {
        snap.asks.push_back(level_summary(price, queue));
    }
    return snap;
}

int64_t OrderBook::qty_at_price(Side side, double price) const {
    const PriceLevels& levels = levels_for(side);
    auto it = levels.find(price);
    if (it == levels.end()) return 0;
    int64_t total = 0;
    for (const RestingOrder& resting : it->second) total += resting.qty;
    return total;
}

std::size_t OrderBook::order_count_at_price(Side side, double price) const {
    const PriceLevels& levels = levels_for(side);
    auto it = levels.find(price);
    return it == levels.end() ? 0 : it->second.size();
}

std::optional<double> OrderBook::best_price(Side side) const {
    const PriceLevels& levels = levels_for(side);
    if (levels.empty()) return std::nullopt;
    return side == Side::kBuy ? levels.rbegin()->first : levels.begin()->first;
}

}  // namespace tse::orderbook
