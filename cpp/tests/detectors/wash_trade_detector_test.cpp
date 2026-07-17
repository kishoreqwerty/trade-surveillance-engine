#include "wash_trade_detector.hpp"

#include <gtest/gtest.h>

#include "account_registry.hpp"
#include "order_book.hpp"

using tse::detectors::AccountRegistry;
using tse::detectors::Entity;
using tse::detectors::WashTradeDetector;
using tse::fix::Execution;
using tse::fix::Order;
using tse::fix::Side;
using tse::orderbook::OrderBook;

namespace {

constexpr const char* kInstrument = "ACME";

Execution make_execution(const std::string& account_id, const std::string& counterparty_account_id) {
    Execution execution;
    execution.trade_id = "EXE-1";
    execution.order_id = "O1";
    execution.account_id = account_id;
    execution.instrument_id = kInstrument;
    execution.side = Side::kBuy;
    execution.price = 100.00;
    execution.qty = 500;
    execution.timestamp_ns = 1000;
    execution.counterparty_account_id = counterparty_account_id;
    execution.venue = "SIM";
    return execution;
}

}  // namespace

TEST(WashTradeDetector, FiresOnSameBeneficialOwnerExecution) {
    AccountRegistry accounts;
    accounts.add(Entity{"ACC-1", "OWNER-A", "client", {}});
    accounts.add(Entity{"ACC-2", "OWNER-A", "client", {}});
    OrderBook book(kInstrument);
    WashTradeDetector detector;

    auto alerts = detector.evaluate(book, make_execution("ACC-1", "ACC-2"), accounts);
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_EQ(alerts[0].detector_name, "WashTradeDetector");
    EXPECT_DOUBLE_EQ(alerts[0].score, 1.0);
    EXPECT_EQ(alerts[0].instrument_id, kInstrument);
    ASSERT_EQ(alerts[0].account_ids.size(), 2u);
    EXPECT_EQ(alerts[0].account_ids[0], "ACC-1");
    EXPECT_EQ(alerts[0].account_ids[1], "ACC-2");
}

TEST(WashTradeDetector, FiresOnExplicitlyLinkedAccountsExecution) {
    AccountRegistry accounts;
    accounts.add(Entity{"ACC-1", "OWNER-A", "client", {"ACC-2"}});
    accounts.add(Entity{"ACC-2", "OWNER-B", "client", {}});
    OrderBook book(kInstrument);
    WashTradeDetector detector;

    auto alerts = detector.evaluate(book, make_execution("ACC-1", "ACC-2"), accounts);
    EXPECT_EQ(alerts.size(), 1u);
}

TEST(WashTradeDetector, FiresOnSelfTrade) {
    AccountRegistry accounts;  // deliberately empty -- self-trade doesn't need registry data
    OrderBook book(kInstrument);
    WashTradeDetector detector;

    auto alerts = detector.evaluate(book, make_execution("ACC-1", "ACC-1"), accounts);
    ASSERT_EQ(alerts.size(), 1u);
    EXPECT_EQ(alerts[0].account_ids[0], "ACC-1");
    EXPECT_EQ(alerts[0].account_ids[1], "ACC-1");
}

TEST(WashTradeDetector, DoesNotFireForUnrelatedAccounts) {
    AccountRegistry accounts;
    accounts.add(Entity{"ACC-1", "OWNER-A", "client", {}});
    accounts.add(Entity{"ACC-2", "OWNER-B", "client", {}});
    OrderBook book(kInstrument);
    WashTradeDetector detector;

    auto alerts = detector.evaluate(book, make_execution("ACC-1", "ACC-2"), accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(WashTradeDetector, DoesNotFireForUnregisteredAccounts) {
    AccountRegistry accounts;  // neither account registered
    OrderBook book(kInstrument);
    WashTradeDetector detector;

    auto alerts = detector.evaluate(book, make_execution("ACC-1", "ACC-2"), accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(WashTradeDetector, DoesNotFireOnEmptyCounterparty) {
    AccountRegistry accounts;
    accounts.add(Entity{"ACC-1", "OWNER-A", "client", {}});
    OrderBook book(kInstrument);
    WashTradeDetector detector;

    auto alerts = detector.evaluate(book, make_execution("ACC-1", ""), accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(WashTradeDetector, IgnoresOrderEventsEntirely) {
    AccountRegistry accounts;
    accounts.add(Entity{"ACC-1", "OWNER-A", "client", {}});
    accounts.add(Entity{"ACC-2", "OWNER-A", "client", {}});
    OrderBook book(kInstrument);
    WashTradeDetector detector;

    Order order;
    order.order_id = "O1";
    order.account_id = "ACC-1";
    order.instrument_id = kInstrument;
    order.side = Side::kBuy;
    order.price = 100.00;
    order.qty = 500;
    order.timestamp_ns = 1000;
    order.status = tse::fix::OrderStatus::kNew;

    auto alerts = detector.evaluate(book, order, accounts);
    EXPECT_TRUE(alerts.empty());
}

TEST(WashTradeDetector, NameIsWashTradeDetector) {
    WashTradeDetector detector;
    EXPECT_EQ(detector.name(), "WashTradeDetector");
}
