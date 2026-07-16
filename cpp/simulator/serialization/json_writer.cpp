#include "serialization/json_writer.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tse::simulator {

namespace {

std::string escape_json(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default: out += c;
        }
    }
    return out;
}

void write_label(std::ostringstream& oss, const GroundTruthLabel& label) {
    oss << "{\"pattern\":\"" << to_string(label.pattern) << "\","
        << "\"scenario_id\":\"" << escape_json(label.scenario_id) << "\","
        << "\"severity\":" << label.severity << "}";
}

void write_order(std::ostringstream& oss, const Order& order) {
    oss << "{";
    oss << "\"order_id\":\"" << escape_json(order.order_id) << "\",";
    oss << "\"account_id\":\"" << escape_json(order.account_id) << "\",";
    oss << "\"instrument_id\":\"" << escape_json(order.instrument_id) << "\",";
    oss << "\"side\":\"" << to_string(order.side) << "\",";
    oss << "\"price\":" << order.price << ",";
    oss << "\"qty\":" << order.qty << ",";
    oss << "\"order_type\":\"" << to_string(order.order_type) << "\",";
    oss << "\"timestamp_ns\":" << order.timestamp_ns << ",";
    oss << "\"status\":\"" << to_string(order.status) << "\",";
    oss << "\"venue\":\"" << escape_json(order.venue) << "\",";
    oss << "\"ground_truth_label\":";
    write_label(oss, order.ground_truth_label);
    oss << "}";
}

void write_execution(std::ostringstream& oss, const Execution& execution) {
    oss << "{";
    oss << "\"trade_id\":\"" << escape_json(execution.trade_id) << "\",";
    oss << "\"order_id\":\"" << escape_json(execution.order_id) << "\",";
    oss << "\"account_id\":\"" << escape_json(execution.account_id) << "\",";
    oss << "\"instrument_id\":\"" << escape_json(execution.instrument_id) << "\",";
    oss << "\"price\":" << execution.price << ",";
    oss << "\"qty\":" << execution.qty << ",";
    oss << "\"timestamp_ns\":" << execution.timestamp_ns << ",";
    oss << "\"counterparty_account_id\":\"" << escape_json(execution.counterparty_account_id) << "\",";
    oss << "\"venue\":\"" << escape_json(execution.venue) << "\",";
    oss << "\"ground_truth_label\":";
    write_label(oss, execution.ground_truth_label);
    oss << "}";
}

// --- Parser -----------------------------------------------------------
//
// Deliberately not a general-purpose JSON parser: it parses exactly the
// object/array/key shape to_labeled_json emits. Numbers keep their raw text
// instead of eagerly converting to double, because timestamp_ns is an
// int64_t epoch-nanosecond value (~1.7e18 for real dates) that exceeds a
// double's 53-bit exact-integer range — going through std::stod would
// silently lose precision on round-trip. as_int64() parses the raw text
// directly via std::stoll instead.

struct JsonValue {
    enum class Kind { kString, kNumber, kObject, kArray } kind{Kind::kString};
    std::string str_value;              // for kString: unescaped value; for kNumber: raw digits
    std::vector<std::pair<std::string, JsonValue>> object_value;
    std::vector<JsonValue> array_value;

    const JsonValue& at(const std::string& key) const {
        for (const auto& [k, v] : object_value) {
            if (k == key) return v;
        }
        throw std::runtime_error("parse_labeled_json: missing key '" + key + "'");
    }

    double as_double() const { return std::stod(str_value); }
    int64_t as_int64() const { return std::stoll(str_value); }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    JsonValue parse_document() { return parse_value(); }

private:
    const std::string& text_;
    size_t pos_{0};

    void skip_ws() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    char peek() {
        skip_ws();
        if (pos_ >= text_.size()) throw std::runtime_error("parse_labeled_json: unexpected end of input");
        return text_[pos_];
    }

    void expect(char c) {
        if (peek() != c) {
            throw std::runtime_error(std::string("parse_labeled_json: expected '") + c + "'");
        }
        ++pos_;
    }

    JsonValue parse_value() {
        char c = peek();
        if (c == '"') return parse_string_value();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        return parse_number();
    }

    std::string parse_raw_string() {
        expect('"');
        std::string out;
        while (pos_ < text_.size() && text_[pos_] != '"') {
            char c = text_[pos_];
            if (c == '\\' && pos_ + 1 < text_.size()) {
                char next = text_[pos_ + 1];
                if (next == '"') { out += '"'; pos_ += 2; continue; }
                if (next == '\\') { out += '\\'; pos_ += 2; continue; }
                if (next == 'n') { out += '\n'; pos_ += 2; continue; }
            }
            out += c;
            ++pos_;
        }
        expect('"');
        return out;
    }

    JsonValue parse_string_value() {
        JsonValue v;
        v.kind = JsonValue::Kind::kString;
        v.str_value = parse_raw_string();
        return v;
    }

    JsonValue parse_number() {
        skip_ws();
        size_t start = pos_;
        while (pos_ < text_.size() &&
               (std::isdigit(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '-' ||
                text_[pos_] == '+' || text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
        }
        if (pos_ == start) throw std::runtime_error("parse_labeled_json: expected a number");
        JsonValue v;
        v.kind = JsonValue::Kind::kNumber;
        v.str_value = text_.substr(start, pos_ - start);
        return v;
    }

    JsonValue parse_object() {
        JsonValue v;
        v.kind = JsonValue::Kind::kObject;
        expect('{');
        if (peek() == '}') { ++pos_; return v; }
        while (true) {
            skip_ws();
            std::string key = parse_raw_string();
            skip_ws();
            expect(':');
            v.object_value.emplace_back(key, parse_value());
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            break;
        }
        expect('}');
        return v;
    }

    JsonValue parse_array() {
        JsonValue v;
        v.kind = JsonValue::Kind::kArray;
        expect('[');
        if (peek() == ']') { ++pos_; return v; }
        while (true) {
            v.array_value.push_back(parse_value());
            skip_ws();
            if (peek() == ',') { ++pos_; continue; }
            break;
        }
        expect(']');
        return v;
    }
};

GroundTruthLabel label_from_json(const JsonValue& v) {
    GroundTruthLabel label;
    label.pattern = abuse_pattern_from_string(v.at("pattern").str_value);
    label.scenario_id = v.at("scenario_id").str_value;
    label.severity = v.at("severity").as_double();
    return label;
}

Order order_from_json(const JsonValue& v) {
    Order order;
    order.order_id = v.at("order_id").str_value;
    order.account_id = v.at("account_id").str_value;
    order.instrument_id = v.at("instrument_id").str_value;
    order.side = side_from_string(v.at("side").str_value);
    order.price = v.at("price").as_double();
    order.qty = v.at("qty").as_int64();
    order.order_type = order_type_from_string(v.at("order_type").str_value);
    order.timestamp_ns = v.at("timestamp_ns").as_int64();
    order.status = order_status_from_string(v.at("status").str_value);
    order.venue = v.at("venue").str_value;
    order.ground_truth_label = label_from_json(v.at("ground_truth_label"));
    return order;
}

Execution execution_from_json(const JsonValue& v) {
    Execution execution;
    execution.trade_id = v.at("trade_id").str_value;
    execution.order_id = v.at("order_id").str_value;
    execution.account_id = v.at("account_id").str_value;
    execution.instrument_id = v.at("instrument_id").str_value;
    execution.price = v.at("price").as_double();
    execution.qty = v.at("qty").as_int64();
    execution.timestamp_ns = v.at("timestamp_ns").as_int64();
    execution.counterparty_account_id = v.at("counterparty_account_id").str_value;
    execution.venue = v.at("venue").str_value;
    execution.ground_truth_label = label_from_json(v.at("ground_truth_label"));
    return execution;
}

}  // namespace

std::string to_labeled_json(const std::vector<Order>& orders, const std::vector<Execution>& executions) {
    std::ostringstream oss;
    // max_digits10 (17) is the number of significant decimal digits needed
    // to round-trip any double exactly — anything less (ostream's default of
    // 6) silently truncates price/severity, as caught by the round-trip test.
    oss << std::setprecision(17);
    oss << "{\"orders\":[";
    for (size_t i = 0; i < orders.size(); ++i) {
        if (i > 0) oss << ",";
        write_order(oss, orders[i]);
    }
    oss << "],\"executions\":[";
    for (size_t i = 0; i < executions.size(); ++i) {
        if (i > 0) oss << ",";
        write_execution(oss, executions[i]);
    }
    oss << "]}";
    return oss.str();
}

ParsedEvents parse_labeled_json(const std::string& json) {
    JsonParser parser(json);
    JsonValue root = parser.parse_document();

    ParsedEvents result;
    for (const auto& item : root.at("orders").array_value) result.orders.push_back(order_from_json(item));
    for (const auto& item : root.at("executions").array_value) {
        result.executions.push_back(execution_from_json(item));
    }
    return result;
}

}  // namespace tse::simulator
