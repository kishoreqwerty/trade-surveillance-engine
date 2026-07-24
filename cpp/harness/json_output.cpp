#include "json_output.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace tse::harness {

JsonWriter& JsonWriter::field(const std::string& key, const std::string& raw_json_value) {
    if (!first_) buf_ += ',';
    first_ = false;
    buf_ += json_string(key);
    buf_ += ':';
    buf_ += raw_json_value;
    return *this;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out += c;
        }
    }
    return out;
}

std::string json_string(const std::string& s) { return '"' + json_escape(s) + '"'; }

std::string json_number(double value) {
    if (std::isnan(value)) return "null";  // matches ConfusionMatrix's own "NaN, not a sentinel" convention
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", value);
    return buf;
}

std::string json_number(int64_t value) { return std::to_string(value); }
std::string json_number(uint64_t value) { return std::to_string(value); }
std::string json_bool(bool value) { return value ? "true" : "false"; }

int64_t now_epoch_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string confusion_matrix_json(const ConfusionMatrix& matrix) {
    JsonWriter w;
    w.field("tp", json_number(matrix.tp));
    w.field("fp", json_number(matrix.fp));
    w.field("fn", json_number(matrix.fn));
    w.field("tn", json_number(matrix.tn));
    w.field("precision", json_number(matrix.precision()));
    w.field("recall", json_number(matrix.recall()));
    w.field("f1", json_number(matrix.f1()));
    return w.str();
}

}  // namespace tse::harness
