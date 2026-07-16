#include <gtest/gtest.h>

#include "serialization/fix_writer.hpp"
#include "types.hpp"

using namespace tse::simulator;

namespace {

unsigned int compute_checksum(const std::string& msg_up_to_checksum_field) {
    unsigned int sum = 0;
    for (unsigned char c : msg_up_to_checksum_field) sum += c;
    return sum % 256;
}

}  // namespace

TEST(FixWriter, NewOrderSingleHasValidChecksumAndBodyLength) {
    Order order;
    order.order_id = "ORD-000001";
    order.account_id = "ACC-000001";
    order.instrument_id = "ACME";
    order.side = Side::kBuy;
    order.price = 101.25;
    order.qty = 500;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = 1'700'000'000'000'000'000LL;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";

    FixMessageBuilder builder;
    std::string msg = builder.build_new_order_single(order);

    EXPECT_NE(msg.find("35=D\x01"), std::string::npos);
    EXPECT_NE(msg.find("11=ORD-000001\x01"), std::string::npos);
    EXPECT_NE(msg.find("54=1\x01"), std::string::npos);  // Buy

    size_t checksum_pos = msg.rfind("10=");
    ASSERT_NE(checksum_pos, std::string::npos);
    unsigned int expected = compute_checksum(msg.substr(0, checksum_pos));
    std::string checksum_str = msg.substr(checksum_pos + 3, 3);
    EXPECT_EQ(std::stoi(checksum_str), static_cast<int>(expected));

    size_t bodylen_tag_pos = msg.find("9=");
    ASSERT_NE(bodylen_tag_pos, std::string::npos);
    size_t bodylen_value_start = bodylen_tag_pos + 2;
    size_t bodylen_soh = msg.find('\x01', bodylen_value_start);
    int declared_body_length =
        std::stoi(msg.substr(bodylen_value_start, bodylen_soh - bodylen_value_start));
    int actual_body_length = static_cast<int>(checksum_pos - (bodylen_soh + 1));
    EXPECT_EQ(declared_body_length, actual_body_length);
}

TEST(FixWriter, OrderCancelRequestUsesMsgTypeF) {
    Order cancel;
    cancel.order_id = "ORD-000002";
    cancel.account_id = "ACC-000001";
    cancel.instrument_id = "ACME";
    cancel.side = Side::kSell;
    cancel.qty = 300;
    cancel.timestamp_ns = 0;
    cancel.status = OrderStatus::kCancelled;

    FixMessageBuilder builder;
    std::string msg = builder.build_order_cancel_request(cancel);
    EXPECT_NE(msg.find("35=F\x01"), std::string::npos);
    EXPECT_NE(msg.find("41=ORD-000002\x01"), std::string::npos);
}

TEST(FixWriter, ExecutionReportUsesMsgType8) {
    Execution execution;
    execution.trade_id = "EXE-000001";
    execution.order_id = "ORD-000001";
    execution.account_id = "ACC-000001";
    execution.instrument_id = "ACME";
    execution.price = 101.25;
    execution.qty = 500;
    execution.timestamp_ns = 0;
    execution.counterparty_account_id = "ACC-000002";
    execution.venue = "SIM";

    FixMessageBuilder builder;
    std::string msg = builder.build_execution_report(execution);
    EXPECT_NE(msg.find("35=8\x01"), std::string::npos);
    EXPECT_NE(msg.find("17=EXE-000001\x01"), std::string::npos);
}

TEST(FixWriter, NeverLeaksGroundTruthLabel) {
    Order order;
    order.order_id = "ORD-000099";
    order.account_id = "ACC-000001";
    order.instrument_id = "ACME";
    order.side = Side::kBuy;
    order.price = 100.0;
    order.qty = 100;
    order.timestamp_ns = 0;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    order.ground_truth_label = {AbusePattern::kSpoofingLayering, "SCN-SPOOF-SECRET-000042", 0.9};

    FixMessageBuilder builder_a;
    std::string msg = builder_a.build_new_order_single(order);

    EXPECT_EQ(msg.find("SCN-SPOOF-SECRET"), std::string::npos);
    EXPECT_EQ(msg.find("SpoofingLayering"), std::string::npos);

    // A stripped copy of the same order produces byte-identical FIX
    // output — the strongest possible proof that FIX serialization carries
    // zero trace of ground truth.
    FixMessageBuilder builder_b;
    std::string stripped_msg = builder_b.build_new_order_single(strip_label(order));
    EXPECT_EQ(msg, stripped_msg);
}

TEST(FixWriter, ToFixMessagesNeverLeaksGroundTruthAcrossASession) {
    Order order;
    order.order_id = "ORD-1";
    order.account_id = "ACC-1";
    order.instrument_id = "ACME";
    order.timestamp_ns = 0;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    order.ground_truth_label = {AbusePattern::kWashTrade, "SCN-WASH-SECRET-000007", 1.0};

    Execution execution;
    execution.order_id = "ORD-1";
    execution.trade_id = "EXE-1";
    execution.account_id = "ACC-1";
    execution.instrument_id = "ACME";
    execution.timestamp_ns = 1;
    execution.venue = "SIM";
    execution.ground_truth_label = {AbusePattern::kWashTrade, "SCN-WASH-SECRET-000007", 1.0};

    auto messages = to_fix_messages({order}, {execution});
    ASSERT_EQ(messages.size(), 2u);
    for (const auto& msg : messages) {
        EXPECT_EQ(msg.find("SCN-WASH-SECRET"), std::string::npos);
        EXPECT_EQ(msg.find("WashTrade"), std::string::npos);
    }
}
