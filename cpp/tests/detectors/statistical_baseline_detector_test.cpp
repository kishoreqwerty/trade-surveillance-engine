#include "statistical_baseline_detector.hpp"

#include <gtest/gtest.h>

#include "account_registry.hpp"
#include "order_book.hpp"

using tse::detectors::AccountRegistry;
using tse::detectors::DetectorEvent;
using tse::detectors::StatisticalBaselineConfig;
using tse::detectors::StatisticalBaselineDetector;
using tse::fix::Order;
using tse::fix::OrderStatus;
using tse::fix::OrderType;
using tse::fix::Side;
using tse::orderbook::OrderBook;

namespace {

constexpr const char* kInstrument = "ACME";

Order make_new(const std::string& id, const std::string& account, int64_t qty, int64_t ts,
               const std::string& instrument = kInstrument) {
    Order order;
    order.order_id = id;
    order.orig_order_id = id;
    order.account_id = account;
    order.instrument_id = instrument;
    order.side = Side::kBuy;
    order.price = 100.00;
    order.qty = qty;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = ts;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    return order;
}

// Feeds the canonical 5-sample baseline (100, 200, 100, 200, 100) for one
// account+instrument. Hand-computed via Welford's recurrence (matching the
// implementation's own update() step exactly): after these 5 samples,
// running mean = 140.0 and sample variance = 3000.0 (stddev =
// sqrt(3000) ~= 54.7723), both exact -- no floating-point surprises at this
// sample size, verified by hand before writing any assertion.
void feed_baseline(OrderBook& book, StatisticalBaselineDetector& detector, const AccountRegistry& accounts,
                    const std::string& account, const std::string& instrument, int64_t& ts) {
    const int64_t values[] = {100, 200, 100, 200, 100};
    for (int64_t v : values) {
        detector.evaluate(book, DetectorEvent{make_new("B-" + std::to_string(ts), account, v, ts, instrument)},
                           accounts);
        ts += 1'000'000'000;
    }
}

}  // namespace

TEST(StatisticalBaselineDetector, NameIsStatisticalBaselineDetector) {
    StatisticalBaselineDetector detector;
    EXPECT_EQ(detector.name(), "StatisticalBaselineDetector");
}

TEST(StatisticalBaselineDetector, InsufficientSampleCountNeverFiresRegardlessOfOutlierSize) {
    OrderBook book(kInstrument);
    StatisticalBaselineDetector detector;  // default min_sample_count: 5
    AccountRegistry accounts;

    int64_t ts = 1'000'000'000;
    for (int64_t v : {100, 200, 100, 200}) {  // only 4 samples
        detector.evaluate(book, DetectorEvent{make_new("O-" + std::to_string(ts), "ACC-1", v, ts)}, accounts);
        ts += 1'000'000'000;
    }
    auto alerts =
        detector.evaluate(book, DetectorEvent{make_new("OUTLIER", "ACC-1", 999'999, ts)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(StatisticalBaselineDetector, AllIdenticalBaselineNeverFiresEvenForExtremeOutlier) {
    OrderBook book(kInstrument);
    StatisticalBaselineDetector detector;
    AccountRegistry accounts;

    int64_t ts = 1'000'000'000;
    for (int i = 0; i < 5; ++i) {  // 5 identical values -> stddev == 0
        detector.evaluate(book, DetectorEvent{make_new("O-" + std::to_string(ts), "ACC-1", 100, ts)}, accounts);
        ts += 1'000'000'000;
    }
    auto alerts =
        detector.evaluate(book, DetectorEvent{make_new("OUTLIER", "ACC-1", 999'999, ts)}, accounts);
    EXPECT_TRUE(alerts.empty());  // stddev == 0 guard prevents a division by zero, not a false alert
}

TEST(StatisticalBaselineDetector, OutlierBeyondThresholdFiresWithHandComputedScore) {
    OrderBook book(kInstrument);
    StatisticalBaselineDetector detector;  // default threshold: 3.0
    AccountRegistry accounts;

    int64_t ts = 1'000'000'000;
    feed_baseline(book, detector, accounts, "ACC-1", kInstrument, ts);
    // mean=140, stddev=sqrt(3000)~=54.7723. z(500) = (500-140)/54.7723 ~= 6.573 -- well past 3.0.
    auto alerts = detector.evaluate(book, DetectorEvent{make_new("OUTLIER", "ACC-1", 500, ts)}, accounts);

    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_EQ(alerts[0].detector_name, "StatisticalBaselineDetector");
    // score = clamp(|z| / (2*threshold), 0, 1) = clamp(6.573/6.0, 0, 1) -- comfortably clamps to 1.0.
    EXPECT_DOUBLE_EQ(alerts[0].score, 1.0);
    EXPECT_EQ(alerts[0].account_ids, std::vector<std::string>{"ACC-1"});
    EXPECT_EQ(alerts[0].order_ids, std::vector<std::string>{"OUTLIER"});
}

TEST(StatisticalBaselineDetector, ValueNearMeanDoesNotFire) {
    OrderBook book(kInstrument);
    StatisticalBaselineDetector detector;
    AccountRegistry accounts;

    int64_t ts = 1'000'000'000;
    feed_baseline(book, detector, accounts, "ACC-1", kInstrument, ts);
    // z(150) = (150-140)/54.7723 ~= 0.183 -- well under 3.0.
    auto alerts = detector.evaluate(book, DetectorEvent{make_new("NEAR", "ACC-1", 150, ts)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(StatisticalBaselineDetector, CustomLowerThresholdFiresOnTheOtherwiseSubthresholdCase) {
    OrderBook book(kInstrument);
    AccountRegistry accounts;
    int64_t ts = 1'000'000'000;

    // z(200) = (200-140)/54.7723 ~= 1.096 -- doesn't clear the default 3.0 threshold.
    {
        StatisticalBaselineDetector default_detector;
        int64_t default_ts = 1'000'000'000;
        feed_baseline(book, default_detector, accounts, "ACC-1", kInstrument, default_ts);
        auto alerts =
            default_detector.evaluate(book, DetectorEvent{make_new("MILD", "ACC-1", 200, default_ts)}, accounts);
        EXPECT_TRUE(alerts.empty());
    }

    // ...but does clear a lowered threshold of 1.0, on a fresh detector/account so the baselines don't mix.
    OrderBook book2(kInstrument);
    StatisticalBaselineConfig config;
    config.z_score_threshold = 1.0;
    StatisticalBaselineDetector lenient_detector(config);
    feed_baseline(book2, lenient_detector, accounts, "ACC-2", kInstrument, ts);
    auto alerts = lenient_detector.evaluate(book2, DetectorEvent{make_new("MILD2", "ACC-2", 200, ts)}, accounts);
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_GT(alerts[0].score, 0.5);
    EXPECT_LT(alerts[0].score, 0.6);
}

TEST(StatisticalBaselineDetector, DifferentAccountHasIndependentBaseline) {
    OrderBook book(kInstrument);
    StatisticalBaselineDetector detector;
    AccountRegistry accounts;

    int64_t ts = 1'000'000'000;
    feed_baseline(book, detector, accounts, "ACC-1", kInstrument, ts);
    // ACC-2 has never traded -- count is 0, well below min_sample_count, regardless of ACC-1's history.
    auto alerts = detector.evaluate(book, DetectorEvent{make_new("FIRST", "ACC-2", 999'999, ts)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(StatisticalBaselineDetector, DifferentInstrumentHasIndependentBaselineForSameAccount) {
    OrderBook book(kInstrument);
    StatisticalBaselineDetector detector;
    AccountRegistry accounts;

    int64_t ts = 1'000'000'000;
    feed_baseline(book, detector, accounts, "ACC-1", kInstrument, ts);
    // ACC-1's first order on a different instrument -- no baseline carried over.
    auto alerts =
        detector.evaluate(book, DetectorEvent{make_new("FIRST", "ACC-1", 999'999, ts, "OTHERINSTR")}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(StatisticalBaselineDetector, IgnoresNonNewOrderAndExecutionEvents) {
    OrderBook book(kInstrument);
    StatisticalBaselineDetector detector;
    AccountRegistry accounts;

    int64_t ts = 1'000'000'000;
    feed_baseline(book, detector, accounts, "ACC-1", kInstrument, ts);

    Order cancel;
    cancel.order_id = "C1";
    cancel.orig_order_id = "B-1";
    cancel.instrument_id = kInstrument;
    cancel.timestamp_ns = ts;
    cancel.status = OrderStatus::kCancelled;
    EXPECT_TRUE(detector.evaluate(book, DetectorEvent{cancel}, accounts).empty());

    tse::fix::Execution execution;
    execution.trade_id = "E1";
    execution.order_id = "B-1";
    execution.account_id = "ACC-1";
    execution.instrument_id = kInstrument;
    execution.side = Side::kBuy;
    execution.price = 100.00;
    execution.qty = 999'999;
    execution.timestamp_ns = ts;
    execution.venue = "SIM";
    EXPECT_TRUE(detector.evaluate(book, DetectorEvent{execution}, accounts).empty());
}
