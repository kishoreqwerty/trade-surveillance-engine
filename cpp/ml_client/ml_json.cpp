#include "ml_json.hpp"

#include <cctype>
#include <sstream>
#include <stdexcept>

namespace tse::ml_client {

namespace {

std::string escape_json_string(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
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

// Finds "key": within a flat JSON object and returns the offset of the
// value that follows, skipping whitespace — or std::string::npos if the
// key isn't present. Scoped to flat, single-level objects, which is all
// this client ever parses (see decode_scoring_response()).
std::size_t find_value_start(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    std::size_t key_pos = body.find(needle);
    if (key_pos == std::string::npos) return std::string::npos;
    std::size_t colon_pos = body.find(':', key_pos + needle.size());
    if (colon_pos == std::string::npos) return std::string::npos;
    std::size_t value_pos = colon_pos + 1;
    while (value_pos < body.size() && std::isspace(static_cast<unsigned char>(body[value_pos]))) {
        ++value_pos;
    }
    return value_pos;
}

std::optional<double> extract_number_field(const std::string& body, const std::string& key) {
    std::size_t start = find_value_start(body, key);
    if (start == std::string::npos) return std::nullopt;
    std::size_t end = start;
    while (end < body.size()) {
        char c = body[end];
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.' || c == 'e' ||
              c == 'E')) {
            break;
        }
        ++end;
    }
    if (end == start) return std::nullopt;
    try {
        return std::stod(body.substr(start, end - start));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<std::string> extract_string_field(const std::string& body, const std::string& key) {
    std::size_t start = find_value_start(body, key);
    if (start == std::string::npos || start >= body.size() || body[start] != '"') return std::nullopt;
    ++start;  // skip opening quote
    std::string result;
    std::size_t i = start;
    while (i < body.size() && body[i] != '"') {
        if (body[i] == '\\' && i + 1 < body.size()) {
            char next = body[i + 1];
            if (next == '"') {
                result += '"';
                i += 2;
                continue;
            }
            if (next == '\\') {
                result += '\\';
                i += 2;
                continue;
            }
            if (next == 'n') {
                result += '\n';
                i += 2;
                continue;
            }
        }
        result += body[i];
        ++i;
    }
    if (i >= body.size()) return std::nullopt;  // unterminated string
    return result;
}

}  // namespace

std::string encode_scoring_request(const ScoringRequest& request) {
    std::ostringstream oss;
    oss.precision(17);
    oss << "{\"account_id\":\"" << escape_json_string(request.account_id) << "\","
        << "\"instrument_id\":\"" << escape_json_string(request.instrument_id) << "\","
        << "\"window_features\":{";
    bool first = true;
    for (const auto& [key, value] : request.window_features) {
        if (!first) oss << ",";
        first = false;
        oss << "\"" << escape_json_string(key) << "\":" << value;
    }
    oss << "}}";
    return oss.str();
}

std::optional<ScoringResult> decode_scoring_response(const std::string& body) {
    std::optional<double> anomaly_score = extract_number_field(body, "anomaly_score");
    std::optional<std::string> model_version = extract_string_field(body, "model_version");
    if (!anomaly_score.has_value() || !model_version.has_value()) return std::nullopt;
    ScoringResult result;
    result.anomaly_score = *anomaly_score;
    result.model_version = std::move(*model_version);
    return result;
}

}  // namespace tse::ml_client
