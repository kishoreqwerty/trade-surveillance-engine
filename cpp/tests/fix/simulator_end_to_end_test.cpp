#include <gtest/gtest.h>

#include <quickfix/Session.h>

#include <chrono>
#include <unordered_map>

#include "fix_session_test_fixture.hpp"
#include "message_translator.hpp"
#include "simulator.hpp"

using tse::fix::testutil::LiveSessionFixture;

namespace {

// QuickFIX's DoubleConvertor serializes Price/Qty fields to 15 significant
// digits (FieldConvertors.h: SIGNIFICANT_DIGITS = 15), not the 17 needed
// for exact IEEE754 double round-trip — a deliberate default, since no real
// instrument's price needs 16+ significant digits. The simulator's
// random-walk price arithmetic accumulates float noise well past what any
// real tick size would carry (e.g. 1.0895337841399073 for an FX rate), so
// this is the only field in this test that needs a tolerance rather than
// exact equality; it's many orders of magnitude tighter than any real tick
// size, so it can't mask an actual translation bug.
constexpr double kPriceWireTolerance = 1e-9;

tse::fix::Order to_fix_order(const tse::simulator::Order& order) {
    tse::fix::Order out;
    out.order_id = order.order_id;
    out.account_id = order.account_id;
    out.instrument_id = order.instrument_id;
    out.side = order.side == tse::simulator::Side::kBuy ? tse::fix::Side::kBuy : tse::fix::Side::kSell;
    out.price = order.price;
    out.qty = order.qty;
    out.order_type =
        order.order_type == tse::simulator::OrderType::kMarket ? tse::fix::OrderType::kMarket : tse::fix::OrderType::kLimit;
    out.timestamp_ns = order.timestamp_ns;
    out.status =
        order.status == tse::simulator::OrderStatus::kCancelled ? tse::fix::OrderStatus::kCancelled : tse::fix::OrderStatus::kNew;
    out.venue = order.venue;
    // Same simplification Phase 1's fix_writer used: the simulator doesn't
    // track a separate cancel-request ID, so the cancelled order's own ID
    // doubles as OrigClOrdID.
    out.orig_order_id = order.order_id;
    return out;
}

tse::fix::Execution to_fix_execution(const tse::simulator::Execution& execution, tse::fix::Side side) {
    tse::fix::Execution out;
    out.trade_id = execution.trade_id;
    out.order_id = execution.order_id;
    out.account_id = execution.account_id;
    out.instrument_id = execution.instrument_id;
    out.side = side;
    out.price = execution.price;
    out.qty = execution.qty;
    out.timestamp_ns = execution.timestamp_ns;
    out.counterparty_account_id = execution.counterparty_account_id;
    out.venue = execution.venue;
    return out;
}

}  // namespace

// This is the literal Phase 1 -> Phase 2 handoff: generate a synthetic
// session with generate_simulation() (untouched Phase 1 code), send every
// resulting Order/Execution as a real FIX message over a genuine
// initiator->acceptor session (untouched Phase 2 session machinery), and
// verify what comes out the other side of fromApp()/message_translator
// matches what went in.
TEST_F(LiveSessionFixture, DrivesPhase1SimulatorOutputThroughARealFixSessionEndToEnd) {
    ASSERT_TRUE(logon());

    tse::simulator::SimulatorConfig config;
    config.random_seed = 123;
    config.session_duration_ns = 30'000'000'000;  // 30s synthetic session — small, fast test
    config.baseline_orders_per_second = 3.0;
    config.num_equity_instruments = 2;
    config.num_fx_instruments = 1;
    config.num_fixed_income_instruments = 0;
    config.num_independent_accounts = 10;
    config.num_linked_account_pairs = 2;
    config.wash_trade = {1, 0.5};

    auto simulation = tse::simulator::generate_simulation(config);
    ASSERT_FALSE(simulation.orders.empty());
    ASSERT_FALSE(simulation.executions.empty());

    // A real ExecutionReport requires Side, which simulator::Execution
    // doesn't carry (see fix/types.hpp's comment on why fix::Execution adds
    // it) — recover it from the originating order, exactly as a real
    // gateway would (an execution always belongs to a specific order side).
    std::unordered_map<std::string, tse::fix::Side> side_by_order_id;
    for (const auto& order : simulation.orders) {
        side_by_order_id[order.order_id] =
            order.side == tse::simulator::Side::kBuy ? tse::fix::Side::kBuy : tse::fix::Side::kSell;
    }

    size_t expected_orders = 0;
    for (const auto& order : simulation.orders) {
        tse::fix::Order fix_order = to_fix_order(order);
        if (order.status == tse::simulator::OrderStatus::kNew) {
            auto message = tse::fix::to_new_order_single(fix_order);
            ASSERT_TRUE(FIX::Session::sendToTarget(message, initiator_session_id_));
            ++expected_orders;
        } else if (order.status == tse::simulator::OrderStatus::kCancelled) {
            auto message = tse::fix::to_order_cancel_request(fix_order);
            ASSERT_TRUE(FIX::Session::sendToTarget(message, initiator_session_id_));
            ++expected_orders;
        }
    }

    for (const auto& execution : simulation.executions) {
        tse::fix::Side side = side_by_order_id.at(execution.order_id);
        auto message = tse::fix::to_execution_report(to_fix_execution(execution, side));
        ASSERT_TRUE(FIX::Session::sendToTarget(message, initiator_session_id_));
    }

    ASSERT_TRUE(acceptor_app_.wait_for_event_count(std::chrono::seconds(15), expected_orders,
                                                    simulation.executions.size()));

    auto received_orders = acceptor_app_.received_orders();
    auto received_executions = acceptor_app_.received_executions();

    EXPECT_EQ(received_orders.size(), expected_orders);
    EXPECT_EQ(received_executions.size(), simulation.executions.size());

    std::unordered_map<std::string, const tse::simulator::Order*> orig_new_by_id;
    std::unordered_map<std::string, const tse::simulator::Order*> orig_cancel_by_id;
    for (const auto& order : simulation.orders) {
        if (order.status == tse::simulator::OrderStatus::kNew) {
            orig_new_by_id[order.order_id] = &order;
        } else if (order.status == tse::simulator::OrderStatus::kCancelled) {
            orig_cancel_by_id[order.order_id] = &order;
        }
    }

    for (const auto& received : received_orders) {
        if (received.status == tse::fix::OrderStatus::kNew) {
            ASSERT_TRUE(orig_new_by_id.count(received.order_id));
            const auto& orig = *orig_new_by_id.at(received.order_id);
            EXPECT_EQ(received.account_id, orig.account_id);
            EXPECT_EQ(received.instrument_id, orig.instrument_id);
            EXPECT_NEAR(received.price, orig.price, kPriceWireTolerance);
            EXPECT_EQ(received.qty, orig.qty);
            EXPECT_EQ(received.timestamp_ns, orig.timestamp_ns);
        } else {
            ASSERT_TRUE(orig_cancel_by_id.count(received.order_id));
            const auto& orig = *orig_cancel_by_id.at(received.order_id);
            EXPECT_EQ(received.account_id, orig.account_id);
            EXPECT_EQ(received.qty, orig.qty);
            EXPECT_EQ(received.timestamp_ns, orig.timestamp_ns);
        }
    }

    std::unordered_map<std::string, const tse::simulator::Execution*> orig_exec_by_id;
    for (const auto& execution : simulation.executions) orig_exec_by_id[execution.trade_id] = &execution;

    for (const auto& received : received_executions) {
        ASSERT_TRUE(orig_exec_by_id.count(received.trade_id));
        const auto& orig = *orig_exec_by_id.at(received.trade_id);
        EXPECT_EQ(received.order_id, orig.order_id);
        EXPECT_EQ(received.account_id, orig.account_id);
        EXPECT_NEAR(received.price, orig.price, kPriceWireTolerance);
        EXPECT_EQ(received.qty, orig.qty);
        EXPECT_EQ(received.timestamp_ns, orig.timestamp_ns);
        EXPECT_EQ(received.counterparty_account_id, orig.counterparty_account_id);
    }
}
