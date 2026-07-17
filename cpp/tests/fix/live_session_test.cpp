#include <gtest/gtest.h>

#include <quickfix/Session.h>

#include <chrono>
#include <thread>

#include "fix_session_test_fixture.hpp"
#include "message_translator.hpp"

using namespace tse::fix;
using tse::fix::testutil::LiveSessionFixture;

namespace {

Order make_new_order(const std::string& id) {
    Order order;
    order.order_id = id;
    order.account_id = "ACC-1";
    order.instrument_id = "ACME";
    order.side = Side::kBuy;
    order.price = 100.25;
    order.qty = 500;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = 1'700'000'000'000'000'000LL;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    return order;
}

}  // namespace

TEST_F(LiveSessionFixture, EstablishesLogonOnBothSides) {
    ASSERT_TRUE(logon());
}

TEST_F(LiveSessionFixture, DeliversAllThreeMessageTypesThroughARealSession) {
    ASSERT_TRUE(logon());

    Order new_order = make_new_order("ORD-LIVE-1");
    FIX42::NewOrderSingle nos = to_new_order_single(new_order);
    ASSERT_TRUE(FIX::Session::sendToTarget(nos, initiator_session_id_));

    Order cancel_order = make_new_order("ORD-LIVE-2");
    cancel_order.orig_order_id = "ORD-LIVE-1";
    cancel_order.status = OrderStatus::kCancelled;
    FIX42::OrderCancelRequest ocr = to_order_cancel_request(cancel_order);
    ASSERT_TRUE(FIX::Session::sendToTarget(ocr, initiator_session_id_));

    Execution execution;
    execution.trade_id = "EXE-LIVE-1";
    execution.order_id = "ORD-LIVE-1";
    execution.account_id = "ACC-1";
    execution.instrument_id = "ACME";
    execution.side = Side::kBuy;
    execution.price = 100.25;
    execution.qty = 500;
    execution.timestamp_ns = 1'700'000'000'500'000'000LL;
    execution.counterparty_account_id = "ACC-2";
    execution.venue = "SIM";
    FIX42::ExecutionReport er = to_execution_report(execution);
    ASSERT_TRUE(FIX::Session::sendToTarget(er, initiator_session_id_));

    ASSERT_TRUE(acceptor_app_.wait_for_event_count(std::chrono::seconds(5), 2, 1));

    auto received_orders = acceptor_app_.received_orders();
    auto received_executions = acceptor_app_.received_executions();

    ASSERT_EQ(received_orders.size(), 2u);
    ASSERT_EQ(received_executions.size(), 1u);

    EXPECT_EQ(received_orders[0].order_id, new_order.order_id);
    EXPECT_EQ(received_orders[0].status, OrderStatus::kNew);
    EXPECT_DOUBLE_EQ(received_orders[0].price, new_order.price);
    EXPECT_EQ(received_orders[0].qty, new_order.qty);
    EXPECT_EQ(received_orders[0].timestamp_ns, new_order.timestamp_ns);
    EXPECT_EQ(received_orders[0].venue, new_order.venue);

    EXPECT_EQ(received_orders[1].order_id, cancel_order.order_id);
    EXPECT_EQ(received_orders[1].orig_order_id, cancel_order.orig_order_id);
    EXPECT_EQ(received_orders[1].status, OrderStatus::kCancelled);
    EXPECT_EQ(received_orders[1].qty, cancel_order.qty);

    EXPECT_EQ(received_executions[0].trade_id, execution.trade_id);
    EXPECT_EQ(received_executions[0].order_id, execution.order_id);
    EXPECT_EQ(received_executions[0].counterparty_account_id, execution.counterparty_account_id);
    EXPECT_DOUBLE_EQ(received_executions[0].price, execution.price);
    EXPECT_EQ(received_executions[0].qty, execution.qty);
    EXPECT_EQ(received_executions[0].timestamp_ns, execution.timestamp_ns);
}

TEST_F(LiveSessionFixture, SequenceGapTriggersResendRequestWithoutCrashing) {
    ASSERT_TRUE(logon());

    Order first = make_new_order("ORD-GAP-0");
    FIX42::NewOrderSingle nos0 = to_new_order_single(first);
    ASSERT_TRUE(FIX::Session::sendToTarget(nos0, initiator_session_id_));
    ASSERT_TRUE(acceptor_app_.wait_for_event_count(std::chrono::seconds(5), 1, 0));

    // Jump the initiator's outbound sequence number forward, creating a gap
    // from the acceptor's point of view — this is QuickFIX's own
    // Session/SessionState, not anything this codebase hand-rolls.
    FIX::Session* initiator_session = FIX::Session::lookupSession(initiator_session_id_);
    ASSERT_NE(initiator_session, nullptr);
    initiator_session->setNextSenderMsgSeqNum(initiator_session->getExpectedSenderNum() + 5);

    Order gapped = make_new_order("ORD-GAP-1");
    FIX42::NewOrderSingle nos1 = to_new_order_single(gapped);
    ASSERT_TRUE(FIX::Session::sendToTarget(nos1, initiator_session_id_));

    // The acceptor notices the gap and *sends* the ResendRequest; the
    // initiator is the one that *receives* it — check the initiator's
    // counter, not the acceptor's. Neither side may crash either way; this
    // is the actual "basic session recovery" requirement, demonstrated
    // rather than asserted.
    for (int i = 0; i < 50 && initiator_app_.resend_requests_seen() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_GT(initiator_app_.resend_requests_seen(), 0);

    // Both sessions are still alive and reachable — nothing crashed or was
    // torn down as a side effect of the gap.
    EXPECT_NE(FIX::Session::lookupSession(acceptor_session_id_), nullptr);
    EXPECT_NE(FIX::Session::lookupSession(initiator_session_id_), nullptr);
}
