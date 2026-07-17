#include "event_codec.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace tse::ingestion {

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

void write_string_field(std::ostringstream& oss, const char* key, const std::string& value, bool comma = true) {
    oss << "\"" << key << "\":\"" << escape_json(value) << "\"";
    if (comma) oss << ",";
}

// --- Minimal flat-object JSON parser --------------------------------------
// Deliberately not general-purpose: parses exactly the flat, single-level
// object shape encode() below produces (string/number values only, no
// nested objects/arrays). Numbers keep their raw text instead of eagerly
// converting to double — timestamp_ns is an int64_t epoch-nanosecond value
// that exceeds a double's 53-bit exact-integer range, so routing it through
// std::stod would silently corrupt it on round-trip (the same pitfall
// documented in cpp/simulator/serialization/json_writer.cpp).
class FlatJsonParser {
public:
    explicit FlatJsonParser(const std::string& text) : text_(text) {}

    std::unordered_map<std::string, std::string> parse_object() {
        std::unordered_map<std::string, std::string> fields;
        expect('{');
        if (peek() == '}') {
            ++pos_;
            return fields;
        }
        while (true) {
            skip_ws();
            std::string key = parse_raw_string();
            skip_ws();
            expect(':');
            skip_ws();
            std::string value = (peek() == '"') ? parse_raw_string() : parse_raw_number();
            fields[key] = value;
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            break;
        }
        expect('}');
        return fields;
    }

private:
    const std::string& text_;
    std::size_t pos_{0};

    void skip_ws() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    char peek() {
        skip_ws();
        if (pos_ >= text_.size()) throw std::runtime_error("event_codec: unexpected end of input");
        return text_[pos_];
    }

    void expect(char c) {
        if (peek() != c) throw std::runtime_error(std::string("event_codec: expected '") + c + "'");
        ++pos_;
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

    std::string parse_raw_number() {
        std::size_t start = pos_;
        while (pos_ < text_.size() &&
               (std::isdigit(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '-' ||
                text_[pos_] == '+' || text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
        }
        if (pos_ == start) throw std::runtime_error("event_codec: expected a number");
        return text_.substr(start, pos_ - start);
    }
};

const std::string& require(const std::unordered_map<std::string, std::string>& fields, const char* key) {
    auto it = fields.find(key);
    if (it == fields.end()) throw std::runtime_error(std::string("event_codec: missing field '") + key + "'");
    return it->second;
}

}  // namespace

std::string encode(const IngestionEvent& event) {
    std::ostringstream oss;
    oss << std::setprecision(17);  // max_digits10 for double — see header comment
    oss << "{";
    if (is_order(event)) {
        const auto& order = std::get<tse::fix::Order>(event);
        write_string_field(oss, "type", "order");
        write_string_field(oss, "order_id", order.order_id);
        write_string_field(oss, "account_id", order.account_id);
        write_string_field(oss, "instrument_id", order.instrument_id);
        write_string_field(oss, "side", tse::fix::to_string(order.side));
        oss << "\"price\":" << order.price << ",";
        oss << "\"qty\":" << order.qty << ",";
        write_string_field(oss, "order_type", tse::fix::to_string(order.order_type));
        oss << "\"timestamp_ns\":" << order.timestamp_ns << ",";
        write_string_field(oss, "status", tse::fix::to_string(order.status));
        write_string_field(oss, "venue", order.venue);
        write_string_field(oss, "orig_order_id", order.orig_order_id, /*comma=*/false);
    } else {
        const auto& execution = std::get<tse::fix::Execution>(event);
        write_string_field(oss, "type", "execution");
        write_string_field(oss, "trade_id", execution.trade_id);
        write_string_field(oss, "order_id", execution.order_id);
        write_string_field(oss, "account_id", execution.account_id);
        write_string_field(oss, "instrument_id", execution.instrument_id);
        write_string_field(oss, "side", tse::fix::to_string(execution.side));
        oss << "\"price\":" << execution.price << ",";
        oss << "\"qty\":" << execution.qty << ",";
        oss << "\"timestamp_ns\":" << execution.timestamp_ns << ",";
        write_string_field(oss, "counterparty_account_id", execution.counterparty_account_id);
        write_string_field(oss, "venue", execution.venue, /*comma=*/false);
    }
    oss << "}";
    return oss.str();
}

IngestionEvent decode(const std::string& json) {
    FlatJsonParser parser(json);
    auto fields = parser.parse_object();
    const std::string& type = require(fields, "type");

    if (type == "order") {
        tse::fix::Order order;
        order.order_id = require(fields, "order_id");
        order.account_id = require(fields, "account_id");
        order.instrument_id = require(fields, "instrument_id");
        order.side = tse::fix::side_from_string(require(fields, "side"));
        order.price = std::stod(require(fields, "price"));
        order.qty = std::stoll(require(fields, "qty"));
        order.order_type = tse::fix::order_type_from_string(require(fields, "order_type"));
        order.timestamp_ns = std::stoll(require(fields, "timestamp_ns"));
        order.status = tse::fix::order_status_from_string(require(fields, "status"));
        order.venue = require(fields, "venue");
        order.orig_order_id = require(fields, "orig_order_id");
        return order;
    }
    if (type == "execution") {
        tse::fix::Execution execution;
        execution.trade_id = require(fields, "trade_id");
        execution.order_id = require(fields, "order_id");
        execution.account_id = require(fields, "account_id");
        execution.instrument_id = require(fields, "instrument_id");
        execution.side = tse::fix::side_from_string(require(fields, "side"));
        execution.price = std::stod(require(fields, "price"));
        execution.qty = std::stoll(require(fields, "qty"));
        execution.timestamp_ns = std::stoll(require(fields, "timestamp_ns"));
        execution.counterparty_account_id = require(fields, "counterparty_account_id");
        execution.venue = require(fields, "venue");
        return execution;
    }
    throw std::runtime_error("event_codec: unrecognized type '" + type + "'");
}

}  // namespace tse::ingestion
