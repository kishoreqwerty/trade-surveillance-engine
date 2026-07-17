#include "marking_the_close_detector.hpp"

#include <gtest/gtest.h>

#include "account_registry.hpp"
#include "order_book.hpp"

using tse::detectors::AccountRegistry;
using tse::detectors::DetectorEvent;
using tse::detectors::MarkingTheCloseConfig;
using tse::detectors::MarkingTheCloseDetector;
using tse::fix::Execution;
using tse::fix::Side;
using tse::orderbook::OrderBook;

namespace {

constexpr const char* kInstrument = "ACME";
constexpr int64_t kCloseTs = 100'000'000'000LL;  // 100s
constexpr int64_t kWindowStart = 90'000'000'000LL;

Execution make_exec(const std::string& account, const std::string& counterparty, int64_t qty, int64_t ts,
                     const std::string& instrument = kInstrument) {
    Execution execution;
    execution.trade_id = "EXE-" + account + "-" + std::to_string(ts);
    execution.order_id = "O-" + account;
    execution.account_id = account;
    execution.instrument_id = instrument;
    execution.side = Side::kBuy;
    execution.price = 100.00;
    execution.qty = qty;
    execution.timestamp_ns = ts;
    execution.counterparty_account_id = counterparty;
    execution.venue = "SIM";
    return execution;
}

MarkingTheCloseConfig default_config() {
    MarkingTheCloseConfig config;
    config.close_time_ns_by_instrument = {{kInstrument, kCloseTs}};
    config.window_duration_ns = kCloseTs - kWindowStart;  // 10s
    config.concentration_threshold = 0.4;
    config.min_account_qty_threshold = 100;
    config.min_total_window_qty_threshold = 500;
    return config;
}

}  // namespace

TEST(MarkingTheCloseDetector, NameIsMarkingTheCloseDetector) {
    MarkingTheCloseDetector detector(default_config());
    EXPECT_EQ(detector.name(), "MarkingTheCloseDetector");
}

// TP: after enough total window volume accrues to be meaningful, a single
// dominant pair's trade pushes both participants' share past threshold.
TEST(MarkingTheCloseDetector, DominantPairCrossingThresholdFires) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;

    // total window volume: 400 -- below the 500 floor, no check attempted.
    EXPECT_TRUE(detector.evaluate(book, DetectorEvent{make_exec("BASE1", "BASE2", 400, 91'000'000'000)}, accounts)
                    .empty());
    // total: 500 -- floor cleared, but BASE3/BASE4 are only 100/500=0.2 each.
    EXPECT_TRUE(detector.evaluate(book, DetectorEvent{make_exec("BASE3", "BASE4", 100, 92'000'000'000)}, accounts)
                    .empty());

    // total: 1100. DOMINANT and OTHER5 each individually own 600/1100 = 0.5454...
    auto alerts = detector.evaluate(book, DetectorEvent{make_exec("DOMINANT", "OTHER5", 600, 95'000'000'000)},
                                     accounts);
    ASSERT_EQ(alerts.size(), 2u);
    EXPECT_EQ(alerts[0].detector_name, "MarkingTheCloseDetector");
    EXPECT_EQ(alerts[0].account_ids, std::vector<std::string>{"DOMINANT"});
    EXPECT_NEAR(alerts[0].score, 600.0 / 1100.0, 1e-9);
    EXPECT_EQ(alerts[1].account_ids, std::vector<std::string>{"OTHER5"});
    EXPECT_NEAR(alerts[1].score, 600.0 / 1100.0, 1e-9);
}

// TN: broad participation -- nobody individually dominates the close even
// once the total-volume floor is cleared.
TEST(MarkingTheCloseDetector, BroadParticipationBelowConcentrationThresholdDoesNotFire) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;

    EXPECT_TRUE(
        detector.evaluate(book, DetectorEvent{make_exec("P1", "P2", 200, 91'000'000'000)}, accounts).empty());
    EXPECT_TRUE(
        detector.evaluate(book, DetectorEvent{make_exec("P3", "P4", 200, 92'000'000'000)}, accounts).empty());
    // total now 600 (floor cleared); P5/P6 each own 200/600 = 0.333 < 0.4.
    auto alerts = detector.evaluate(book, DetectorEvent{make_exec("P5", "P6", 200, 93'000'000'000)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(MarkingTheCloseDetector, ExecutionBeforeWindowIsIgnored) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;

    auto alerts =
        detector.evaluate(book, DetectorEvent{make_exec("DOMINANT", "OTHER", 100'000, 50'000'000'000)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(MarkingTheCloseDetector, ExecutionAfterCloseIsIgnored) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;

    auto alerts =
        detector.evaluate(book, DetectorEvent{make_exec("DOMINANT", "OTHER", 100'000, kCloseTs + 1)}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(MarkingTheCloseDetector, UnknownInstrumentNeverFires) {
    OrderBook book("UNKNOWN");
    MarkingTheCloseDetector detector(default_config());  // only knows ACME's close time
    AccountRegistry accounts;

    auto alerts = detector.evaluate(
        book, DetectorEvent{make_exec("DOMINANT", "OTHER", 100'000, 95'000'000'000, "UNKNOWN")}, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(MarkingTheCloseDetector, AlreadyAlertedAccountDoesNotReAlertOnSubsequentQualifyingExecution) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;

    detector.evaluate(book, DetectorEvent{make_exec("BASE1", "BASE2", 400, 91'000'000'000)}, accounts);
    detector.evaluate(book, DetectorEvent{make_exec("BASE3", "BASE4", 100, 92'000'000'000)}, accounts);
    auto first = detector.evaluate(book, DetectorEvent{make_exec("DOMINANT", "OTHER5", 600, 95'000'000'000)},
                                    accounts);
    ASSERT_EQ(first.size(), 2u);  // both DOMINANT and OTHER5 fire the first time

    // DOMINANT and OTHER5 trade again -- both would still individually
    // qualify by share, but both already fired once.
    auto second = detector.evaluate(book, DetectorEvent{make_exec("DOMINANT", "OTHER5", 200, 96'000'000'000)},
                                     accounts);
    EXPECT_TRUE(second.empty());
}

TEST(MarkingTheCloseDetector, IgnoresOrderEventsEntirely) {
    OrderBook book(kInstrument);
    MarkingTheCloseDetector detector(default_config());
    AccountRegistry accounts;

    tse::fix::Order order;
    order.order_id = "O1";
    order.account_id = "DOMINANT";
    order.instrument_id = kInstrument;
    order.side = Side::kBuy;
    order.price = 100.00;
    order.qty = 100'000;
    order.timestamp_ns = 95'000'000'000;
    order.status = tse::fix::OrderStatus::kNew;

    auto alerts = detector.evaluate(book, DetectorEvent{order}, accounts);
    EXPECT_TRUE(alerts.empty());
}
