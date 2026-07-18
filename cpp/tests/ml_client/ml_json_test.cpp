#include "ml_json.hpp"

#include <gtest/gtest.h>

using tse::ml_client::decode_scoring_response;
using tse::ml_client::encode_scoring_request;
using tse::ml_client::ScoringRequest;

TEST(MlJson, EncodeRequestProducesExpectedShape) {
    ScoringRequest request;
    request.account_id = "ACC-1";
    request.instrument_id = "ACME";
    request.window_features = {{"order_count", 25.0}, {"total_qty", 3750.0}};

    std::string json = encode_scoring_request(request);
    EXPECT_NE(json.find(R"("account_id":"ACC-1")"), std::string::npos);
    EXPECT_NE(json.find(R"("instrument_id":"ACME")"), std::string::npos);
    EXPECT_NE(json.find(R"("window_features":{)"), std::string::npos);
    EXPECT_NE(json.find(R"("order_count":25)"), std::string::npos);
    EXPECT_NE(json.find(R"("total_qty":3750)"), std::string::npos);
}

TEST(MlJson, EncodeEmptyWindowFeaturesProducesEmptyObject) {
    ScoringRequest request;
    request.account_id = "ACC-1";
    request.instrument_id = "ACME";
    std::string json = encode_scoring_request(request);
    EXPECT_NE(json.find(R"("window_features":{}})"), std::string::npos);
}

TEST(MlJson, EncodeEscapesQuotesAndBackslashes) {
    ScoringRequest request;
    request.account_id = R"(ACC-"quoted"-\slash)";
    request.instrument_id = "ACME";
    std::string json = encode_scoring_request(request);
    EXPECT_NE(json.find(R"(ACC-\"quoted\"-\\slash)"), std::string::npos);
}

TEST(MlJson, DecodeValidResponseSucceeds) {
    auto result = decode_scoring_response(R"({"anomaly_score":0.73,"model_version":"isoforest-abc123"})");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->anomaly_score, 0.73);
    EXPECT_EQ(result->model_version, "isoforest-abc123");
}

TEST(MlJson, DecodeHandlesWhitespaceAndReversedKeyOrder) {
    auto result = decode_scoring_response(R"({ "model_version" : "v2" , "anomaly_score" : 0.1 })");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->anomaly_score, 0.1);
    EXPECT_EQ(result->model_version, "v2");
}

TEST(MlJson, DecodeRoundTripsNegativeAndExponentNumbers) {
    auto result = decode_scoring_response(R"({"anomaly_score":-1.5e-3,"model_version":"v1"})");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->anomaly_score, -1.5e-3);
}

TEST(MlJson, DecodeRejectsMissingAnomalyScore) {
    EXPECT_FALSE(decode_scoring_response(R"({"model_version":"v1"})").has_value());
}

TEST(MlJson, DecodeRejectsMissingModelVersion) {
    EXPECT_FALSE(decode_scoring_response(R"({"anomaly_score":0.5})").has_value());
}

TEST(MlJson, DecodeRejectsGarbage) {
    EXPECT_FALSE(decode_scoring_response("not json at all").has_value());
    EXPECT_FALSE(decode_scoring_response("").has_value());
}
