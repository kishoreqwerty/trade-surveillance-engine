#include "json_output.hpp"

#include <cmath>

#include <gtest/gtest.h>

using tse::harness::ConfusionMatrix;
using tse::harness::confusion_matrix_json;
using tse::harness::json_bool;
using tse::harness::json_escape;
using tse::harness::json_number;
using tse::harness::json_string;
using tse::harness::JsonWriter;

TEST(JsonOutput, EscapesQuotesBackslashesAndNewlines) {
    EXPECT_EQ(json_escape("plain"), "plain");
    EXPECT_EQ(json_escape("has \"quotes\""), "has \\\"quotes\\\"");
    EXPECT_EQ(json_escape("back\\slash"), "back\\\\slash");
    EXPECT_EQ(json_escape("line\nbreak"), "line\\nbreak");
}

TEST(JsonOutput, JsonStringWrapsInQuotesAndEscapes) { EXPECT_EQ(json_string("a\"b"), "\"a\\\"b\""); }

TEST(JsonOutput, JsonNumberDoubleFormatsWithSixDecimals) { EXPECT_EQ(json_number(0.5), "0.500000"); }

TEST(JsonOutput, JsonNumberDoubleEmitsNullForNaN) {
    // Matches ConfusionMatrix::precision()/recall()/f1()'s own "NaN, not a
    // sentinel value" convention -- a --json consumer must be able to tell
    // "never fired" apart from "fired perfectly," the same reason
    // report.cpp renders NaN as "n/a" rather than 0.0 or 1.0.
    EXPECT_EQ(json_number(std::nan("")), "null");
}

TEST(JsonOutput, JsonNumberIntegerTypesFormatAsPlainDigits) {
    EXPECT_EQ(json_number(static_cast<int64_t>(-42)), "-42");
    EXPECT_EQ(json_number(static_cast<uint64_t>(42)), "42");
}

TEST(JsonOutput, JsonBoolFormatsAsBareTrueOrFalse) {
    EXPECT_EQ(json_bool(true), "true");
    EXPECT_EQ(json_bool(false), "false");
}

TEST(JsonOutput, WriterJoinsFieldsWithCommasAndNoTrailingComma) {
    JsonWriter w;
    w.field("a", json_number(static_cast<int64_t>(1)));
    w.field("b", json_string("x"));
    EXPECT_EQ(w.str(), R"({"a":1,"b":"x"})");
}

TEST(JsonOutput, WriterWithNoFieldsProducesEmptyObject) {
    JsonWriter w;
    EXPECT_EQ(w.str(), "{}");
}

TEST(JsonOutput, ConfusionMatrixJsonIncludesAllSevenFields) {
    ConfusionMatrix m;
    m.tp = 10;
    m.fp = 2;
    m.fn = 3;
    m.tn = 100;
    const std::string json = confusion_matrix_json(m);
    EXPECT_NE(json.find(R"("tp":10)"), std::string::npos);
    EXPECT_NE(json.find(R"("fp":2)"), std::string::npos);
    EXPECT_NE(json.find(R"("fn":3)"), std::string::npos);
    EXPECT_NE(json.find(R"("tn":100)"), std::string::npos);
    EXPECT_NE(json.find("\"precision\":"), std::string::npos);
    EXPECT_NE(json.find("\"recall\":"), std::string::npos);
    EXPECT_NE(json.find("\"f1\":"), std::string::npos);
}

TEST(JsonOutput, ConfusionMatrixJsonWithZeroDenominatorEmitsNullPrecision) {
    ConfusionMatrix m;  // tp=fp=0 -- precision() is NaN by this struct's own convention
    const std::string json = confusion_matrix_json(m);
    EXPECT_NE(json.find(R"("precision":null)"), std::string::npos);
}

TEST(JsonOutput, NowEpochNsReturnsAPlausibleCurrentTimestamp) {
    // Sanity bound, not an exact check -- roughly "some time after this
    // project's own kEpochAnchorNs constants," which is enough to catch a
    // unit mixup (seconds vs. nanos) without pinning to wall-clock time.
    const int64_t ns = tse::harness::now_epoch_ns();
    EXPECT_GT(ns, 1'700'000'000'000'000'000LL);
}
