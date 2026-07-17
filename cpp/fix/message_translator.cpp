#include "message_translator.hpp"

#include <quickfix/FixValues.h>

#include <cmath>
#include <stdexcept>

namespace tse::fix {

namespace {

char side_to_fix(Side side) { return side == Side::kBuy ? FIX::Side_BUY : FIX::Side_SELL; }

Side side_from_fix(char value) {
    if (value == FIX::Side_BUY) return Side::kBuy;
    if (value == FIX::Side_SELL) return Side::kSell;
    throw std::invalid_argument("side_from_fix: unrecognized FIX Side value");
}

char order_type_to_fix(OrderType type) {
    return type == OrderType::kMarket ? FIX::OrdType_MARKET : FIX::OrdType_LIMIT;
}

OrderType order_type_from_fix(char value) {
    if (value == FIX::OrdType_MARKET) return OrderType::kMarket;
    if (value == FIX::OrdType_LIMIT) return OrderType::kLimit;
    throw std::invalid_argument("order_type_from_fix: unrecognized FIX OrdType value");
}

// FIX::UtcTimeStamp stores full nanosecond precision internally (DateTime's
// m_time is nanoseconds-since-midnight), and this fork's field classes
// accept an explicit wire-serialization precision — request 9 (nanoseconds)
// so TransactTime round-trips exactly instead of silently truncating to
// FIX 4.2's traditional millisecond convention.
constexpr int kTimestampPrecision = 9;

FIX::TransactTime to_transact_time(int64_t timestamp_ns) {
    int64_t whole_seconds = timestamp_ns / 1'000'000'000;
    int64_t nanos = timestamp_ns % 1'000'000'000;
    if (nanos < 0) {
        nanos += 1'000'000'000;
        --whole_seconds;
    }
    FIX::UtcTimeStamp value(static_cast<time_t>(whole_seconds), static_cast<int>(nanos), kTimestampPrecision);
    return FIX::TransactTime(value, kTimestampPrecision);
}

int64_t from_transact_time(const FIX::TransactTime& field) {
    const FIX::UtcTimeStamp& value = field.getValue();
    int64_t seconds = static_cast<int64_t>(value.getTimeT());
    int64_t nanos = value.getFraction(kTimestampPrecision);
    return seconds * 1'000'000'000 + nanos;
}

int64_t round_to_int64(double value) { return static_cast<int64_t>(std::llround(value)); }

}  // namespace

FIX42::NewOrderSingle to_new_order_single(const Order& order) {
    FIX42::NewOrderSingle message(FIX::ClOrdID(order.order_id),
                                   FIX::HandlInst(FIX::HandlInst_AUTOMATED_EXECUTION_NO_INTERVENTION),
                                   FIX::Symbol(order.instrument_id), FIX::Side(side_to_fix(order.side)),
                                   to_transact_time(order.timestamp_ns),
                                   FIX::OrdType(order_type_to_fix(order.order_type)));
    message.set(FIX::Account(order.account_id));
    message.set(FIX::OrderQty(static_cast<double>(order.qty)));
    message.set(FIX::Price(order.price));
    message.set(FIX::ExDestination(order.venue));
    return message;
}

Order from_new_order_single(const FIX42::NewOrderSingle& message) {
    Order order;

    FIX::ClOrdID clOrdID;
    message.get(clOrdID);
    order.order_id = clOrdID.getValue();

    FIX::Account account;
    message.get(account);
    order.account_id = account.getValue();

    FIX::Symbol symbol;
    message.get(symbol);
    order.instrument_id = symbol.getValue();

    FIX::Side side;
    message.get(side);
    order.side = side_from_fix(side.getValue());

    FIX::Price price;
    message.get(price);
    order.price = price.getValue();

    FIX::OrderQty qty;
    message.get(qty);
    order.qty = round_to_int64(qty.getValue());

    FIX::OrdType ordType;
    message.get(ordType);
    order.order_type = order_type_from_fix(ordType.getValue());

    FIX::TransactTime transactTime;
    message.get(transactTime);
    order.timestamp_ns = from_transact_time(transactTime);

    FIX::ExDestination venue;
    message.get(venue);
    order.venue = venue.getValue();

    order.status = OrderStatus::kNew;
    return order;
}

FIX42::OrderCancelRequest to_order_cancel_request(const Order& order) {
    FIX42::OrderCancelRequest message(FIX::OrigClOrdID(order.orig_order_id), FIX::ClOrdID(order.order_id),
                                       FIX::Symbol(order.instrument_id), FIX::Side(side_to_fix(order.side)),
                                       to_transact_time(order.timestamp_ns));
    message.set(FIX::Account(order.account_id));
    message.set(FIX::OrderQty(static_cast<double>(order.qty)));
    return message;
}

Order from_order_cancel_request(const FIX42::OrderCancelRequest& message) {
    Order order;

    FIX::ClOrdID clOrdID;
    message.get(clOrdID);
    order.order_id = clOrdID.getValue();

    FIX::OrigClOrdID origClOrdID;
    message.get(origClOrdID);
    order.orig_order_id = origClOrdID.getValue();

    FIX::Account account;
    message.get(account);
    order.account_id = account.getValue();

    FIX::Symbol symbol;
    message.get(symbol);
    order.instrument_id = symbol.getValue();

    FIX::Side side;
    message.get(side);
    order.side = side_from_fix(side.getValue());

    FIX::OrderQty qty;
    message.get(qty);
    order.qty = round_to_int64(qty.getValue());

    FIX::TransactTime transactTime;
    message.get(transactTime);
    order.timestamp_ns = from_transact_time(transactTime);

    order.status = OrderStatus::kCancelled;
    // FIX 4.2's OrderCancelRequest has no Price, OrdType, or ExDestination
    // field at all (confirmed against the generated message class) — these
    // simply aren't transmitted on a cancel request, so they come back as
    // defaults, not because of any parsing gap.
    order.price = 0.0;
    order.order_type = OrderType::kLimit;
    order.venue.clear();
    return order;
}

FIX42::ExecutionReport to_execution_report(const Execution& execution) {
    FIX42::ExecutionReport message(
        FIX::OrderID(execution.order_id), FIX::ExecID(execution.trade_id),
        FIX::ExecTransType(FIX::ExecTransType_NEW), FIX::ExecType(FIX::ExecType_FILL),
        FIX::OrdStatus(FIX::OrdStatus_FILLED), FIX::Symbol(execution.instrument_id),
        FIX::Side(side_to_fix(execution.side)), FIX::LeavesQty(0),
        FIX::CumQty(static_cast<double>(execution.qty)), FIX::AvgPx(execution.price));
    message.set(FIX::Account(execution.account_id));
    message.set(FIX::ClOrdID(execution.order_id));
    message.set(FIX::LastShares(static_cast<double>(execution.qty)));
    message.set(FIX::LastPx(execution.price));
    message.set(FIX::LastMkt(execution.venue));
    message.set(to_transact_time(execution.timestamp_ns));

    // Counterparty account travels in the standard NoContraBrokers repeating
    // group (tag 382 / ContraBroker 375) — there's no flat "counterparty"
    // field on ExecutionReport in FIX 4.2.
    FIX42::ExecutionReport::NoContraBrokers contra_group;
    contra_group.set(FIX::ContraBroker(execution.counterparty_account_id));
    message.addGroup(contra_group);

    return message;
}

Execution from_execution_report(const FIX42::ExecutionReport& message) {
    Execution execution;

    FIX::ExecID execID;
    message.get(execID);
    execution.trade_id = execID.getValue();

    FIX::OrderID orderID;
    message.get(orderID);
    execution.order_id = orderID.getValue();

    FIX::Account account;
    message.get(account);
    execution.account_id = account.getValue();

    FIX::Symbol symbol;
    message.get(symbol);
    execution.instrument_id = symbol.getValue();

    FIX::Side side;
    message.get(side);
    execution.side = side_from_fix(side.getValue());

    FIX::LastPx lastPx;
    message.get(lastPx);
    execution.price = lastPx.getValue();

    FIX::LastShares lastShares;
    message.get(lastShares);
    execution.qty = round_to_int64(lastShares.getValue());

    FIX::TransactTime transactTime;
    message.get(transactTime);
    execution.timestamp_ns = from_transact_time(transactTime);

    FIX::LastMkt lastMkt;
    message.get(lastMkt);
    execution.venue = lastMkt.getValue();

    FIX42::ExecutionReport::NoContraBrokers contra_group;
    message.getGroup(1, contra_group);
    FIX::ContraBroker contraBroker;
    contra_group.get(contraBroker);
    execution.counterparty_account_id = contraBroker.getValue();

    return execution;
}

}  // namespace tse::fix
