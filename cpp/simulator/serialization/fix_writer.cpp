#include "serialization/fix_writer.hpp"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace tse::simulator {

namespace {

constexpr char kSoh = '\x01';

std::string format_utc_timestamp(int64_t timestamp_ns) {
    int64_t whole_seconds = timestamp_ns / 1'000'000'000;
    int64_t millis = (timestamp_ns % 1'000'000'000) / 1'000'000;
    if (millis < 0) millis += 1000;

    std::time_t t = static_cast<std::time_t>(whole_seconds);
    std::tm tm_utc{};
    gmtime_r(&t, &tm_utc);

    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y%m%d-%H:%M:%S") << "." << std::setw(3) << std::setfill('0') << millis;
    return oss.str();
}

std::string format_price(double price) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << price;
    return oss.str();
}

const char* side_to_fix(Side side) { return side == Side::kBuy ? "1" : "2"; }

const char* ord_type_to_fix(OrderType type) { return type == OrderType::kMarket ? "1" : "2"; }

}  // namespace

FixMessageBuilder::FixMessageBuilder(std::string sender_comp_id, std::string target_comp_id)
    : sender_comp_id_(std::move(sender_comp_id)), target_comp_id_(std::move(target_comp_id)) {}

std::string FixMessageBuilder::assemble(const std::string& msg_type, const std::string& body_fields,
                                         int64_t timestamp_ns) {
    std::string header_after_bodylen;
    header_after_bodylen += "35=" + msg_type + kSoh;
    header_after_bodylen += "49=" + sender_comp_id_ + kSoh;
    header_after_bodylen += "56=" + target_comp_id_ + kSoh;
    header_after_bodylen += "34=" + std::to_string(seq_num_++) + kSoh;
    header_after_bodylen += "52=" + format_utc_timestamp(timestamp_ns) + kSoh;
    header_after_bodylen += body_fields;

    std::string prefix = "8=FIX.4.2" + std::string(1, kSoh) + "9=" +
                          std::to_string(header_after_bodylen.size()) + kSoh;

    std::string full_without_checksum = prefix + header_after_bodylen;

    unsigned int checksum = 0;
    for (unsigned char c : full_without_checksum) checksum += c;
    checksum %= 256;

    std::ostringstream checksum_field;
    checksum_field << "10=" << std::setw(3) << std::setfill('0') << checksum << kSoh;

    return full_without_checksum + checksum_field.str();
}

std::string FixMessageBuilder::build_new_order_single(const Order& order) {
    std::string body;
    body += "11=" + order.order_id + kSoh;
    body += "1=" + order.account_id + kSoh;
    body += "55=" + order.instrument_id + kSoh;
    body += "54=" + std::string(side_to_fix(order.side)) + kSoh;
    body += "38=" + std::to_string(order.qty) + kSoh;
    body += "40=" + std::string(ord_type_to_fix(order.order_type)) + kSoh;
    body += "44=" + format_price(order.price) + kSoh;
    body += "60=" + format_utc_timestamp(order.timestamp_ns) + kSoh;
    body += "100=" + order.venue + kSoh;
    return assemble("D", body, order.timestamp_ns);
}

std::string FixMessageBuilder::build_order_cancel_request(const Order& cancel_order) {
    std::string body;
    body += "11=" + cancel_order.order_id + kSoh;
    body += "41=" + cancel_order.order_id + kSoh;
    body += "1=" + cancel_order.account_id + kSoh;
    body += "55=" + cancel_order.instrument_id + kSoh;
    body += "54=" + std::string(side_to_fix(cancel_order.side)) + kSoh;
    body += "38=" + std::to_string(cancel_order.qty) + kSoh;
    body += "60=" + format_utc_timestamp(cancel_order.timestamp_ns) + kSoh;
    return assemble("F", body, cancel_order.timestamp_ns);
}

std::string FixMessageBuilder::build_execution_report(const Execution& execution) {
    std::string body;
    body += "37=" + execution.order_id + kSoh;
    body += "17=" + execution.trade_id + kSoh;
    body += "1=" + execution.account_id + kSoh;
    body += "55=" + execution.instrument_id + kSoh;
    body += "150=F" + std::string(1, kSoh);  // ExecType=Trade
    body += "39=2" + std::string(1, kSoh);   // OrdStatus=Filled (simplification: no partial-fill tracking here)
    body += "32=" + std::to_string(execution.qty) + kSoh;
    body += "31=" + format_price(execution.price) + kSoh;
    body += "14=" + std::to_string(execution.qty) + kSoh;
    body += "151=0" + std::string(1, kSoh);
    body += "6=" + format_price(execution.price) + kSoh;
    body += "60=" + format_utc_timestamp(execution.timestamp_ns) + kSoh;
    body += "100=" + execution.venue + kSoh;
    return assemble("8", body, execution.timestamp_ns);
}

std::vector<std::string> to_fix_messages(const std::vector<Order>& orders,
                                          const std::vector<Execution>& executions) {
    struct Item {
        int64_t timestamp_ns;
        bool is_execution;
        size_t index;
    };

    std::vector<Item> items;
    items.reserve(orders.size() + executions.size());
    for (size_t i = 0; i < orders.size(); ++i) {
        if (orders[i].status == OrderStatus::kReplaced) continue;
        items.push_back({orders[i].timestamp_ns, false, i});
    }
    for (size_t i = 0; i < executions.size(); ++i) {
        items.push_back({executions[i].timestamp_ns, true, i});
    }
    std::stable_sort(items.begin(), items.end(),
                      [](const Item& a, const Item& b) { return a.timestamp_ns < b.timestamp_ns; });

    FixMessageBuilder builder;
    std::vector<std::string> messages;
    messages.reserve(items.size());
    for (const auto& item : items) {
        if (item.is_execution) {
            messages.push_back(builder.build_execution_report(executions[item.index]));
        } else {
            const Order& order = orders[item.index];
            if (order.status == OrderStatus::kNew) {
                messages.push_back(builder.build_new_order_single(order));
            } else if (order.status == OrderStatus::kCancelled) {
                messages.push_back(builder.build_order_cancel_request(order));
            }
        }
    }
    return messages;
}

}  // namespace tse::simulator
