#include <gtest/gtest.h>

#include "event_codec.hpp"

using namespace tse::ingestion;
using namespace tse::fix;

TEST(EventCodec, OrderRoundTripsAllFields) {
    Order order;
    order.order_id = "ORD-000123";
    order.account_id = "ACC-000045";
    order.instrument_id = "ACME";
    order.side = Side::kSell;
    order.price = 101.12345678901234;
    order.qty = 4200;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = 1'700'000'000'123'456'789LL;  // exceeds double's 53-bit exact range
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    order.orig_order_id = "ORD-000100";

    IngestionEvent event = order;
    std::string json = encode(event);
    IngestionEvent round_tripped = decode(json);

    ASSERT_TRUE(is_order(round_tripped));
    const Order& out = std::get<Order>(round_tripped);
    EXPECT_EQ(out.order_id, order.order_id);
    EXPECT_EQ(out.account_id, order.account_id);
    EXPECT_EQ(out.instrument_id, order.instrument_id);
    EXPECT_EQ(out.side, order.side);
    EXPECT_DOUBLE_EQ(out.price, order.price);
    EXPECT_EQ(out.qty, order.qty);
    EXPECT_EQ(out.order_type, order.order_type);
    EXPECT_EQ(out.timestamp_ns, order.timestamp_ns);
    EXPECT_EQ(out.status, order.status);
    EXPECT_EQ(out.venue, order.venue);
    EXPECT_EQ(out.orig_order_id, order.orig_order_id);
}

TEST(EventCodec, ExecutionRoundTripsAllFields) {
    Execution execution;
    execution.trade_id = "EXE-000999";
    execution.order_id = "ORD-000123";
    execution.account_id = "ACC-000045";
    execution.instrument_id = "ACME";
    execution.side = Side::kBuy;
    execution.price = 99.87654321987654;
    execution.qty = 300;
    execution.timestamp_ns = 1'700'000'010'250'000'001LL;
    execution.counterparty_account_id = "ACC-000099";
    execution.venue = "SIM";

    IngestionEvent event = execution;
    std::string json = encode(event);
    IngestionEvent round_tripped = decode(json);

    ASSERT_TRUE(is_execution(round_tripped));
    const Execution& out = std::get<Execution>(round_tripped);
    EXPECT_EQ(out.trade_id, execution.trade_id);
    EXPECT_EQ(out.order_id, execution.order_id);
    EXPECT_EQ(out.account_id, execution.account_id);
    EXPECT_EQ(out.instrument_id, execution.instrument_id);
    EXPECT_EQ(out.side, execution.side);
    EXPECT_DOUBLE_EQ(out.price, execution.price);
    EXPECT_EQ(out.qty, execution.qty);
    EXPECT_EQ(out.timestamp_ns, execution.timestamp_ns);
    EXPECT_EQ(out.counterparty_account_id, execution.counterparty_account_id);
    EXPECT_EQ(out.venue, execution.venue);
}

TEST(EventCodec, DecodeRejectsUnrecognizedType) {
    EXPECT_THROW(decode(R"({"type":"nonsense"})"), std::runtime_error);
}

TEST(EventCodec, DecodeRejectsMissingRequiredField) {
    EXPECT_THROW(decode(R"({"type":"order","order_id":"ORD-1"})"), std::runtime_error);
}
