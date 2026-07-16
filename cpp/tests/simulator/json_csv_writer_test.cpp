#include <gtest/gtest.h>

#include <algorithm>

#include "serialization/csv_writer.hpp"
#include "serialization/json_writer.hpp"
#include "types.hpp"

using namespace tse::simulator;

namespace {
Order make_labeled_order() {
    Order order;
    order.order_id = "ORD-000001";
    order.account_id = "ACC-000001";
    order.instrument_id = "ACME";
    order.side = Side::kBuy;
    order.price = 101.5;
    order.qty = 500;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = 123;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    order.ground_truth_label = {AbusePattern::kMarkingTheClose, "SCN-MTC-000005", 0.75};
    return order;
}

Execution make_labeled_execution() {
    Execution execution;
    execution.trade_id = "EXE-000001";
    execution.order_id = "ORD-000001";
    execution.account_id = "ACC-000001";
    execution.instrument_id = "ACME";
    execution.price = 101.5;
    execution.qty = 500;
    execution.timestamp_ns = 124;
    execution.counterparty_account_id = "ACC-000002";
    execution.venue = "SIM";
    execution.ground_truth_label = {AbusePattern::kMarkingTheClose, "SCN-MTC-000005", 0.75};
    return execution;
}
}  // namespace

TEST(JsonWriter, LabeledJsonRecoversGroundTruth) {
    std::string json = to_labeled_json({make_labeled_order()}, {make_labeled_execution()});
    EXPECT_NE(json.find("\"pattern\":\"MarkingTheClose\""), std::string::npos);
    EXPECT_NE(json.find("\"scenario_id\":\"SCN-MTC-000005\""), std::string::npos);
    EXPECT_NE(json.find("\"severity\":0.75"), std::string::npos);
    EXPECT_NE(json.find("\"order_id\":\"ORD-000001\""), std::string::npos);
}

TEST(JsonWriter, BaselineEventsCarryBaselineSentinel) {
    Order order;
    order.order_id = "ORD-BASE";
    std::string json = to_labeled_json({order}, {});
    EXPECT_NE(json.find("\"pattern\":\"Baseline\""), std::string::npos);
    EXPECT_NE(json.find("\"scenario_id\":\"\""), std::string::npos);
}

TEST(CsvWriter, OrdersCsvHasOneRowPerOrderPlusHeader) {
    std::string csv = orders_to_csv({make_labeled_order(), make_labeled_order()});
    int line_count = static_cast<int>(std::count(csv.begin(), csv.end(), '\n'));
    EXPECT_EQ(line_count, 3);  // header + 2 rows
    EXPECT_NE(csv.find("MarkingTheClose"), std::string::npos);
    EXPECT_NE(csv.find("SCN-MTC-000005"), std::string::npos);
}

TEST(CsvWriter, ExecutionsCsvHasOneRowPerExecutionPlusHeader) {
    std::string csv = executions_to_csv({make_labeled_execution()});
    int line_count = static_cast<int>(std::count(csv.begin(), csv.end(), '\n'));
    EXPECT_EQ(line_count, 2);  // header + 1 row
    EXPECT_NE(csv.find("SCN-MTC-000005"), std::string::npos);
}

// --- True round-trip tests: serialize -> parse -> compare against the
// original struct. The earlier tests above only check that label text
// appears somewhere in the serialized output; these actually deserialize it
// back and diff every field, including ground_truth_label.
//
// timestamp_ns is deliberately set to a realistic epoch-nanosecond value
// (~1.7e18) here, which exceeds a double's 53-bit exact-integer range
// (~9e15). If the parser round-tripped int64 fields through std::stod this
// would silently corrupt the timestamp; asserting exact equality catches
// that class of bug.

TEST(JsonWriter, RoundTripPreservesGroundTruthAndAllFields) {
    Order order = make_labeled_order();
    order.timestamp_ns = 1'700'000'000'123'456'789LL;
    order.qty = 123'456'789'012LL;
    order.price = 101.12345678;

    Execution execution = make_labeled_execution();
    execution.timestamp_ns = 1'700'000'000'987'654'321LL;
    execution.qty = 987'654'321LL;
    execution.price = 99.87654321;

    std::string json = to_labeled_json({order}, {execution});
    ParsedEvents parsed = parse_labeled_json(json);

    ASSERT_EQ(parsed.orders.size(), 1u);
    ASSERT_EQ(parsed.executions.size(), 1u);

    const Order& parsed_order = parsed.orders[0];
    EXPECT_EQ(parsed_order.order_id, order.order_id);
    EXPECT_EQ(parsed_order.account_id, order.account_id);
    EXPECT_EQ(parsed_order.instrument_id, order.instrument_id);
    EXPECT_EQ(parsed_order.side, order.side);
    EXPECT_DOUBLE_EQ(parsed_order.price, order.price);
    EXPECT_EQ(parsed_order.qty, order.qty);
    EXPECT_EQ(parsed_order.order_type, order.order_type);
    EXPECT_EQ(parsed_order.timestamp_ns, order.timestamp_ns);
    EXPECT_EQ(parsed_order.status, order.status);
    EXPECT_EQ(parsed_order.venue, order.venue);
    EXPECT_EQ(parsed_order.ground_truth_label.pattern, order.ground_truth_label.pattern);
    EXPECT_EQ(parsed_order.ground_truth_label.scenario_id, order.ground_truth_label.scenario_id);
    EXPECT_DOUBLE_EQ(parsed_order.ground_truth_label.severity, order.ground_truth_label.severity);

    const Execution& parsed_execution = parsed.executions[0];
    EXPECT_EQ(parsed_execution.trade_id, execution.trade_id);
    EXPECT_EQ(parsed_execution.order_id, execution.order_id);
    EXPECT_DOUBLE_EQ(parsed_execution.price, execution.price);
    EXPECT_EQ(parsed_execution.qty, execution.qty);
    EXPECT_EQ(parsed_execution.timestamp_ns, execution.timestamp_ns);
    EXPECT_EQ(parsed_execution.counterparty_account_id, execution.counterparty_account_id);
    EXPECT_EQ(parsed_execution.ground_truth_label.pattern, execution.ground_truth_label.pattern);
    EXPECT_EQ(parsed_execution.ground_truth_label.scenario_id, execution.ground_truth_label.scenario_id);
    EXPECT_DOUBLE_EQ(parsed_execution.ground_truth_label.severity, execution.ground_truth_label.severity);
}

TEST(JsonWriter, RoundTripCoversEveryAbusePatternAndBaseline) {
    std::vector<AbusePattern> patterns = {AbusePattern::kBaseline, AbusePattern::kWashTrade,
                                           AbusePattern::kSpoofingLayering, AbusePattern::kMarkingTheClose,
                                           AbusePattern::kFrontRunning};
    std::vector<Order> orders;
    for (size_t i = 0; i < patterns.size(); ++i) {
        Order order = make_labeled_order();
        order.order_id = "ORD-" + std::to_string(i);
        order.ground_truth_label = {patterns[i], patterns[i] == AbusePattern::kBaseline ? "" : "SCN-" + std::to_string(i),
                                     0.1 * static_cast<double>(i)};
        orders.push_back(order);
    }

    ParsedEvents parsed = parse_labeled_json(to_labeled_json(orders, {}));

    ASSERT_EQ(parsed.orders.size(), orders.size());
    for (size_t i = 0; i < orders.size(); ++i) {
        EXPECT_EQ(parsed.orders[i].ground_truth_label.pattern, orders[i].ground_truth_label.pattern);
        EXPECT_EQ(parsed.orders[i].ground_truth_label.scenario_id, orders[i].ground_truth_label.scenario_id);
        EXPECT_DOUBLE_EQ(parsed.orders[i].ground_truth_label.severity, orders[i].ground_truth_label.severity);
    }
}

TEST(CsvWriter, RoundTripPreservesGroundTruthAndAllFields) {
    Order order = make_labeled_order();
    order.timestamp_ns = 1'700'000'000'123'456'789LL;
    order.qty = 123'456'789'012LL;
    order.price = 101.12345678;

    Execution execution = make_labeled_execution();
    execution.timestamp_ns = 1'700'000'000'987'654'321LL;
    execution.qty = 987'654'321LL;
    execution.price = 99.87654321;

    auto parsed_orders = parse_orders_csv(orders_to_csv({order}));
    auto parsed_executions = parse_executions_csv(executions_to_csv({execution}));

    ASSERT_EQ(parsed_orders.size(), 1u);
    ASSERT_EQ(parsed_executions.size(), 1u);

    const Order& parsed_order = parsed_orders[0];
    EXPECT_EQ(parsed_order.order_id, order.order_id);
    EXPECT_EQ(parsed_order.side, order.side);
    EXPECT_DOUBLE_EQ(parsed_order.price, order.price);
    EXPECT_EQ(parsed_order.qty, order.qty);
    EXPECT_EQ(parsed_order.order_type, order.order_type);
    EXPECT_EQ(parsed_order.timestamp_ns, order.timestamp_ns);
    EXPECT_EQ(parsed_order.status, order.status);
    EXPECT_EQ(parsed_order.ground_truth_label.pattern, order.ground_truth_label.pattern);
    EXPECT_EQ(parsed_order.ground_truth_label.scenario_id, order.ground_truth_label.scenario_id);
    EXPECT_DOUBLE_EQ(parsed_order.ground_truth_label.severity, order.ground_truth_label.severity);

    const Execution& parsed_execution = parsed_executions[0];
    EXPECT_EQ(parsed_execution.trade_id, execution.trade_id);
    EXPECT_DOUBLE_EQ(parsed_execution.price, execution.price);
    EXPECT_EQ(parsed_execution.qty, execution.qty);
    EXPECT_EQ(parsed_execution.timestamp_ns, execution.timestamp_ns);
    EXPECT_EQ(parsed_execution.ground_truth_label.pattern, execution.ground_truth_label.pattern);
    EXPECT_EQ(parsed_execution.ground_truth_label.scenario_id, execution.ground_truth_label.scenario_id);
    EXPECT_DOUBLE_EQ(parsed_execution.ground_truth_label.severity, execution.ground_truth_label.severity);
}

TEST(CsvWriter, RoundTripCoversEveryAbusePatternAndBaseline) {
    std::vector<AbusePattern> patterns = {AbusePattern::kBaseline, AbusePattern::kWashTrade,
                                           AbusePattern::kSpoofingLayering, AbusePattern::kMarkingTheClose,
                                           AbusePattern::kFrontRunning};
    std::vector<Order> orders;
    for (size_t i = 0; i < patterns.size(); ++i) {
        Order order = make_labeled_order();
        order.order_id = "ORD-" + std::to_string(i);
        order.ground_truth_label = {patterns[i], patterns[i] == AbusePattern::kBaseline ? "" : "SCN-" + std::to_string(i),
                                     0.1 * static_cast<double>(i)};
        orders.push_back(order);
    }

    auto parsed = parse_orders_csv(orders_to_csv(orders));

    ASSERT_EQ(parsed.size(), orders.size());
    for (size_t i = 0; i < orders.size(); ++i) {
        EXPECT_EQ(parsed[i].ground_truth_label.pattern, orders[i].ground_truth_label.pattern);
        EXPECT_EQ(parsed[i].ground_truth_label.scenario_id, orders[i].ground_truth_label.scenario_id);
        EXPECT_DOUBLE_EQ(parsed[i].ground_truth_label.severity, orders[i].ground_truth_label.severity);
    }
}
