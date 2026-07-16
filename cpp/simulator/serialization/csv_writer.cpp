#include "serialization/csv_writer.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace tse::simulator {

namespace {

std::string csv_escape(const std::string& value) {
    bool needs_quoting = value.find(',') != std::string::npos || value.find('"') != std::string::npos ||
                          value.find('\n') != std::string::npos;
    if (!needs_quoting) return value;

    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

// Splits one CSV line into fields, honoring quoted fields with "" escaping —
// the inverse of csv_escape above.
std::vector<std::string> parse_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    size_t i = 0;
    while (true) {
        std::string field;
        if (i < line.size() && line[i] == '"') {
            ++i;
            while (i < line.size()) {
                if (line[i] == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') {
                        field += '"';
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                field += line[i];
                ++i;
            }
        } else {
            while (i < line.size() && line[i] != ',') {
                field += line[i];
                ++i;
            }
        }
        fields.push_back(field);
        if (i < line.size() && line[i] == ',') {
            ++i;
            continue;
        }
        break;
    }
    return fields;
}

std::vector<std::string> split_lines(const std::string& csv) {
    std::vector<std::string> lines;
    std::istringstream iss(csv);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

}  // namespace

std::string orders_to_csv(const std::vector<Order>& orders) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(8);
    oss << "order_id,account_id,instrument_id,side,price,qty,order_type,timestamp_ns,status,venue,"
           "gt_pattern,gt_scenario_id,gt_severity\n";
    for (const auto& order : orders) {
        oss << csv_escape(order.order_id) << "," << csv_escape(order.account_id) << ","
            << csv_escape(order.instrument_id) << "," << to_string(order.side) << "," << order.price << ","
            << order.qty << "," << to_string(order.order_type) << "," << order.timestamp_ns << ","
            << to_string(order.status) << "," << csv_escape(order.venue) << ","
            << to_string(order.ground_truth_label.pattern) << ","
            << csv_escape(order.ground_truth_label.scenario_id) << "," << order.ground_truth_label.severity
            << "\n";
    }
    return oss.str();
}

std::string executions_to_csv(const std::vector<Execution>& executions) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(8);
    oss << "trade_id,order_id,account_id,instrument_id,price,qty,timestamp_ns,counterparty_account_id,venue,"
           "gt_pattern,gt_scenario_id,gt_severity\n";
    for (const auto& execution : executions) {
        oss << csv_escape(execution.trade_id) << "," << csv_escape(execution.order_id) << ","
            << csv_escape(execution.account_id) << "," << csv_escape(execution.instrument_id) << ","
            << execution.price << "," << execution.qty << "," << execution.timestamp_ns << ","
            << csv_escape(execution.counterparty_account_id) << "," << csv_escape(execution.venue) << ","
            << to_string(execution.ground_truth_label.pattern) << ","
            << csv_escape(execution.ground_truth_label.scenario_id) << ","
            << execution.ground_truth_label.severity << "\n";
    }
    return oss.str();
}

std::vector<Order> parse_orders_csv(const std::string& csv) {
    std::vector<Order> orders;
    auto lines = split_lines(csv);
    if (lines.empty()) return orders;

    // lines[0] is the header.
    for (size_t i = 1; i < lines.size(); ++i) {
        auto f = parse_csv_line(lines[i]);
        if (f.size() != 13) {
            throw std::runtime_error("parse_orders_csv: expected 13 columns, got " + std::to_string(f.size()));
        }
        Order order;
        order.order_id = f[0];
        order.account_id = f[1];
        order.instrument_id = f[2];
        order.side = side_from_string(f[3]);
        order.price = std::stod(f[4]);
        order.qty = std::stoll(f[5]);
        order.order_type = order_type_from_string(f[6]);
        order.timestamp_ns = std::stoll(f[7]);
        order.status = order_status_from_string(f[8]);
        order.venue = f[9];
        order.ground_truth_label.pattern = abuse_pattern_from_string(f[10]);
        order.ground_truth_label.scenario_id = f[11];
        order.ground_truth_label.severity = std::stod(f[12]);
        orders.push_back(order);
    }
    return orders;
}

std::vector<Execution> parse_executions_csv(const std::string& csv) {
    std::vector<Execution> executions;
    auto lines = split_lines(csv);
    if (lines.empty()) return executions;

    for (size_t i = 1; i < lines.size(); ++i) {
        auto f = parse_csv_line(lines[i]);
        if (f.size() != 12) {
            throw std::runtime_error("parse_executions_csv: expected 12 columns, got " +
                                      std::to_string(f.size()));
        }
        Execution execution;
        execution.trade_id = f[0];
        execution.order_id = f[1];
        execution.account_id = f[2];
        execution.instrument_id = f[3];
        execution.price = std::stod(f[4]);
        execution.qty = std::stoll(f[5]);
        execution.timestamp_ns = std::stoll(f[6]);
        execution.counterparty_account_id = f[7];
        execution.venue = f[8];
        execution.ground_truth_label.pattern = abuse_pattern_from_string(f[9]);
        execution.ground_truth_label.scenario_id = f[10];
        execution.ground_truth_label.severity = std::stod(f[11]);
        executions.push_back(execution);
    }
    return executions;
}

}  // namespace tse::simulator
