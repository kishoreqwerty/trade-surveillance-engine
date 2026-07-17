#include <gtest/gtest.h>

#include <quickfix/DataDictionary.h>

#include "message_translator.hpp"

using namespace tse::fix;

TEST(MessageTranslator, NewOrderSingleRoundTripsAllFieldsSellLimit) {
    Order original;
    original.order_id = "ORD-000123";
    original.account_id = "ACC-000045";
    original.instrument_id = "ACME";
    original.side = Side::kSell;
    original.price = 101.125;
    original.qty = 4200;
    original.order_type = OrderType::kLimit;
    original.timestamp_ns = 1'700'000'000'123'456'789LL;
    original.status = OrderStatus::kNew;
    original.venue = "SIM";

    FIX42::NewOrderSingle message = to_new_order_single(original);
    Order round_tripped = from_new_order_single(message);

    EXPECT_EQ(round_tripped.order_id, original.order_id);
    EXPECT_EQ(round_tripped.account_id, original.account_id);
    EXPECT_EQ(round_tripped.instrument_id, original.instrument_id);
    EXPECT_EQ(round_tripped.side, original.side);
    EXPECT_DOUBLE_EQ(round_tripped.price, original.price);
    EXPECT_EQ(round_tripped.qty, original.qty);
    EXPECT_EQ(round_tripped.order_type, original.order_type);
    EXPECT_EQ(round_tripped.timestamp_ns, original.timestamp_ns);
    EXPECT_EQ(round_tripped.status, original.status);
    EXPECT_EQ(round_tripped.venue, original.venue);
}

TEST(MessageTranslator, NewOrderSingleRoundTripsAllFieldsBuyMarket) {
    Order original;
    original.order_id = "ORD-000456";
    original.account_id = "ACC-000078";
    original.instrument_id = "EURUSD";
    original.side = Side::kBuy;
    original.price = 1.08345;
    original.qty = 100000;
    original.order_type = OrderType::kMarket;
    original.timestamp_ns = 42;  // small timestamp, edge case near epoch
    original.status = OrderStatus::kNew;
    original.venue = "SIM2";

    FIX42::NewOrderSingle message = to_new_order_single(original);
    Order round_tripped = from_new_order_single(message);

    EXPECT_EQ(round_tripped.order_id, original.order_id);
    EXPECT_EQ(round_tripped.account_id, original.account_id);
    EXPECT_EQ(round_tripped.instrument_id, original.instrument_id);
    EXPECT_EQ(round_tripped.side, original.side);
    EXPECT_DOUBLE_EQ(round_tripped.price, original.price);
    EXPECT_EQ(round_tripped.qty, original.qty);
    EXPECT_EQ(round_tripped.order_type, original.order_type);
    EXPECT_EQ(round_tripped.timestamp_ns, original.timestamp_ns);
    EXPECT_EQ(round_tripped.status, original.status);
    EXPECT_EQ(round_tripped.venue, original.venue);
}

TEST(MessageTranslator, TransactTimeNanosecondPrecisionSurvivesWireSerialization) {
    // Proves precision fidelity at the actual wire-text level, not just
    // object-to-object: build the message, render it to its FIX string,
    // re-parse that string into a fresh message object, and only then
    // extract. If nanosecond precision were silently dropped somewhere in
    // serialization, this is what would catch it (an object-to-object
    // round trip alone would not, if getValue() ever returned a cached
    // object rather than something derived from the wire string).
    Order original;
    original.order_id = "ORD-1";
    original.account_id = "ACC-1";
    original.instrument_id = "ACME";
    original.side = Side::kBuy;
    original.price = 100.0;
    original.qty = 100;
    original.order_type = OrderType::kLimit;
    original.timestamp_ns = 1'700'000'000'123'456'789LL;
    original.status = OrderStatus::kNew;
    original.venue = "SIM";

    FIX42::NewOrderSingle message = to_new_order_single(original);
    std::string wire = message.toString();

    FIX42::NewOrderSingle reparsed;
    reparsed.setString(wire, false);  // pure parse, skip DataDictionary validation

    Order round_tripped = from_new_order_single(reparsed);
    EXPECT_EQ(round_tripped.timestamp_ns, original.timestamp_ns);
}

TEST(MessageTranslator, OrderCancelRequestRoundTripsTransmittedFieldsAndDropsUntransmittedOnes) {
    Order original;
    original.order_id = "ORD-000200";        // this cancel request's own ClOrdID
    original.orig_order_id = "ORD-000123";   // the order being cancelled
    original.account_id = "ACC-000045";
    original.instrument_id = "ACME";
    original.side = Side::kSell;
    original.qty = 4200;
    original.timestamp_ns = 1'700'000'005'000'000'000LL;
    original.status = OrderStatus::kCancelled;
    // Set values FIX 4.2's OrderCancelRequest cannot carry on the wire, to
    // prove the round trip genuinely drops them rather than "getting lucky"
    // with matching defaults. Confirmed against the generated message
    // class (fix42/OrderCancelRequest.h): it has no Price, OrdType, or
    // ExDestination field at all.
    original.price = 999.99;
    original.order_type = OrderType::kMarket;
    original.venue = "SIM";

    FIX42::OrderCancelRequest message = to_order_cancel_request(original);
    Order round_tripped = from_order_cancel_request(message);

    EXPECT_EQ(round_tripped.order_id, original.order_id);
    EXPECT_EQ(round_tripped.orig_order_id, original.orig_order_id);
    EXPECT_EQ(round_tripped.account_id, original.account_id);
    EXPECT_EQ(round_tripped.instrument_id, original.instrument_id);
    EXPECT_EQ(round_tripped.side, original.side);
    EXPECT_EQ(round_tripped.qty, original.qty);
    EXPECT_EQ(round_tripped.timestamp_ns, original.timestamp_ns);
    EXPECT_EQ(round_tripped.status, OrderStatus::kCancelled);

    EXPECT_DOUBLE_EQ(round_tripped.price, 0.0);
    EXPECT_EQ(round_tripped.order_type, OrderType::kLimit);
    EXPECT_EQ(round_tripped.venue, "");
}

TEST(MessageTranslator, ExecutionReportRoundTripsAllFields) {
    Execution original;
    original.trade_id = "EXE-000999";
    original.order_id = "ORD-000123";
    original.account_id = "ACC-000045";
    original.instrument_id = "ACME";
    original.side = Side::kBuy;
    original.price = 101.125;
    original.qty = 300;
    original.timestamp_ns = 1'700'000'010'250'000'000LL;
    original.counterparty_account_id = "ACC-000099";
    original.venue = "SIM";

    FIX42::ExecutionReport message = to_execution_report(original);
    Execution round_tripped = from_execution_report(message);

    EXPECT_EQ(round_tripped.trade_id, original.trade_id);
    EXPECT_EQ(round_tripped.order_id, original.order_id);
    EXPECT_EQ(round_tripped.account_id, original.account_id);
    EXPECT_EQ(round_tripped.instrument_id, original.instrument_id);
    EXPECT_EQ(round_tripped.side, original.side);
    EXPECT_DOUBLE_EQ(round_tripped.price, original.price);
    EXPECT_EQ(round_tripped.qty, original.qty);
    EXPECT_EQ(round_tripped.timestamp_ns, original.timestamp_ns);
    EXPECT_EQ(round_tripped.counterparty_account_id, original.counterparty_account_id);
    EXPECT_EQ(round_tripped.venue, original.venue);
}

TEST(MessageTranslator, ExecutionReportRoundTripsAllFieldsSellSide) {
    Execution original;
    original.trade_id = "EXE-000111";
    original.order_id = "ORD-000222";
    original.account_id = "ACC-000010";
    original.instrument_id = "UST10Y";
    original.side = Side::kSell;
    original.price = 99.5;
    original.qty = 5000;
    original.timestamp_ns = 1;  // smallest nonzero, edge case
    original.counterparty_account_id = "ACC-000011";
    original.venue = "SIM";

    FIX42::ExecutionReport message = to_execution_report(original);
    Execution round_tripped = from_execution_report(message);

    EXPECT_EQ(round_tripped.trade_id, original.trade_id);
    EXPECT_EQ(round_tripped.side, original.side);
    EXPECT_DOUBLE_EQ(round_tripped.price, original.price);
    EXPECT_EQ(round_tripped.qty, original.qty);
    EXPECT_EQ(round_tripped.timestamp_ns, original.timestamp_ns);
    EXPECT_EQ(round_tripped.counterparty_account_id, original.counterparty_account_id);
}

// Same wire-level rigor as the NewOrderSingle precision test above, applied
// to ExecutionReport's NoContraBrokers repeating group — proves
// counterparty_account_id survives actual FIX text serialization, not just
// in-memory object copies.
TEST(MessageTranslator, ExecutionReportContraBrokerSurvivesWireSerialization) {
    Execution original;
    original.trade_id = "EXE-1";
    original.order_id = "ORD-1";
    original.account_id = "ACC-1";
    original.instrument_id = "ACME";
    original.side = Side::kBuy;
    original.price = 100.0;
    original.qty = 100;
    original.timestamp_ns = 1'700'000'000'000'000'000LL;
    original.counterparty_account_id = "ACC-COUNTERPARTY-42";
    original.venue = "SIM";

    FIX42::ExecutionReport message = to_execution_report(original);
    std::string wire = message.toString();

    // Unlike flat fields, a repeating group's boundaries (where
    // NoContraBrokers(382) ends and the next top-level field begins) aren't
    // recoverable from raw tag=value text alone — the parser needs a
    // DataDictionary to know ContraBroker(375) belongs inside that group.
    // A real session always has one configured (see fix_session_test_fixture);
    // this test provides it explicitly since it parses outside a session.
    FIX::DataDictionary data_dictionary(QUICKFIX_FIX42_SPEC_PATH);
    FIX42::ExecutionReport reparsed;
    reparsed.setString(wire, false, &data_dictionary, &data_dictionary);

    Execution round_tripped = from_execution_report(reparsed);
    EXPECT_EQ(round_tripped.counterparty_account_id, original.counterparty_account_id);
}
