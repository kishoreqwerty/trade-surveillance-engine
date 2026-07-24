#pragma once

#include <cstdint>
#include <string>

#include "evaluation.hpp"

namespace tse::harness {

// A minimal, purpose-built JSON string builder for exactly the fixed
// shapes tse_harness_eval/tse_sarao_validation's --json output needs --
// same "hand-roll exactly the encoding a fixed, known shape needs"
// precedent as cpp/simulator/serialization/json_writer.cpp and
// AlertStore's text-array encoding, not a general-purpose JSON library
// pulled in for two small, known-shape files. Deliberately not crow::json
// (that would make a non-HTTP evaluation binary depend on an HTTP
// framework library for output formatting).
class JsonWriter {
public:
    JsonWriter() { buf_ += '{'; }

    // Writes `"key":value` (or a leading comma first, for every entry
    // after the first) -- callers pass already-JSON-encoded fragments
    // (numbers, quoted strings, nested {..}/[..] blocks built the same
    // way), not raw values, so this class stays a thin joiner rather than
    // a type-aware encoder.
    JsonWriter& field(const std::string& key, const std::string& raw_json_value);

    std::string str() const { return buf_ + '}'; }

private:
    std::string buf_;
    bool first_{true};
};

std::string json_escape(const std::string& s);
std::string json_string(const std::string& s);  // a quoted, escaped JSON string literal
std::string json_number(double value);           // "null" for NaN -- see ConfusionMatrix's own NaN convention
std::string json_number(int64_t value);
std::string json_number(uint64_t value);
std::string json_bool(bool value);

// Epoch nanoseconds, matching this project's own "all timestamps ...
// int64_t epoch nanos" convention (CLAUDE.md) -- used as every --json
// output's generated_at_unix_ns field, so the dashboard can show "harness
// snapshot generated <time>" distinctly from its genuinely live tabs.
int64_t now_epoch_ns();

// The one ConfusionMatrix -> JSON fragment shared by every --json caller
// (evaluation.json's per-detector entries, spoofing_rate_sweep.json's
// per-rate points) -- built once here instead of once per call site.
std::string confusion_matrix_json(const ConfusionMatrix& matrix);

}  // namespace tse::harness
