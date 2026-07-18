#include "alert_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>

#include "db_test_helpers.hpp"

using tse::db::AlertStore;
using tse::db::StoredAlert;
using tse::db::testutil::connect_or_skip;
using tse::detectors::Alert;

namespace {

Alert make_alert(const std::string& detector_name, const std::vector<std::string>& account_ids,
                  int64_t window_start_ns) {
    Alert alert;
    alert.detector_name = detector_name;
    alert.score = 0.9;
    alert.instrument_id = "ACME";
    alert.account_ids = account_ids;
    alert.order_ids = {"O1"};
    alert.window_start_ns = window_start_ns;
    alert.window_end_ns = window_start_ns + 1000;
    alert.evidence = "test evidence for " + detector_name;
    return alert;
}

bool contains_detector(const std::vector<StoredAlert>& alerts, const std::string& detector_name) {
    return std::any_of(alerts.begin(), alerts.end(),
                        [&](const StoredAlert& a) { return a.alert.detector_name == detector_name; });
}

}  // namespace

// The build guide's Phase 8 "Done when" names three query shapes
// explicitly: time-range, filter-by-account, filter-by-detector-type. Each
// test below is built so there's at least one alert that must be excluded
// as well as one that must be included -- a query that degenerated into
// "return everything" would fail these just as loudly as one that returned
// nothing.
class AlertStoreQueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        store_ = connect_or_skip();
        if (!store_) GTEST_SKIP() << "TimescaleDB not reachable at 127.0.0.1:5432 -- run `docker compose up -d timescaledb` first.";

        store_->insert_alert(make_alert("WashTradeDetector", {"ACC-1"}, 1'000'000'000LL));
        store_->insert_alert(make_alert("SpoofingLayeringDetector", {"ACC-2"}, 2'000'000'000LL));
        store_->insert_alert(make_alert("WashTradeDetector", {"ACC-1", "ACC-3"}, 3'000'000'000LL));
        store_->insert_alert(make_alert("FrontRunningDetector", {"ACC-4"}, 4'000'000'000LL));
    }

    std::unique_ptr<AlertStore> store_;
};

TEST_F(AlertStoreQueryTest, TimeRangeReturnsOnlyAlertsWithinBounds) {
    std::vector<StoredAlert> found = store_->query_alerts_by_time_range(1'500'000'000LL, 3'500'000'000LL);
    ASSERT_EQ(found.size(), 2u);
    EXPECT_TRUE(contains_detector(found, "SpoofingLayeringDetector"));
    EXPECT_TRUE(contains_detector(found, "WashTradeDetector"));
    EXPECT_FALSE(contains_detector(found, "FrontRunningDetector"));
    for (const auto& stored : found) {
        EXPECT_GE(stored.alert.window_start_ns, 1'500'000'000LL);
        EXPECT_LE(stored.alert.window_start_ns, 3'500'000'000LL);
    }
}

TEST_F(AlertStoreQueryTest, TimeRangeOutsideAllAlertsReturnsEmpty) {
    std::vector<StoredAlert> found = store_->query_alerts_by_time_range(10'000'000'000LL, 20'000'000'000LL);
    EXPECT_TRUE(found.empty());
}

TEST_F(AlertStoreQueryTest, FilterByAccountFindsAlertsWhereAccountIsAnyMember) {
    // ACC-1 is a sole member of one alert and one of two members of
    // another -- both must come back; the SpoofingLayeringDetector/
    // FrontRunningDetector alerts (different accounts entirely) must not.
    std::vector<StoredAlert> found = store_->query_alerts_by_account("ACC-1");
    ASSERT_EQ(found.size(), 2u);
    for (const auto& stored : found) {
        EXPECT_NE(std::find(stored.alert.account_ids.begin(), stored.alert.account_ids.end(), "ACC-1"),
                  stored.alert.account_ids.end());
    }
}

TEST_F(AlertStoreQueryTest, FilterByAccountWithNoMatchesReturnsEmpty) {
    std::vector<StoredAlert> found = store_->query_alerts_by_account("ACC-DOES-NOT-EXIST");
    EXPECT_TRUE(found.empty());
}

TEST_F(AlertStoreQueryTest, FilterByDetectorTypeReturnsOnlyThatDetector) {
    std::vector<StoredAlert> found = store_->query_alerts_by_detector("WashTradeDetector");
    ASSERT_EQ(found.size(), 2u);
    for (const auto& stored : found) {
        EXPECT_EQ(stored.alert.detector_name, "WashTradeDetector");
    }
}

TEST_F(AlertStoreQueryTest, FilterByDetectorTypeWithNoMatchesReturnsEmpty) {
    std::vector<StoredAlert> found = store_->query_alerts_by_detector("MarkingTheCloseDetector");
    EXPECT_TRUE(found.empty());
}
