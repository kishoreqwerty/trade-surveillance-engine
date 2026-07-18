// The live demo server: the actual "Done when" entrypoint for Phase 9 --
// "the dashboard, running against the live pipeline, correctly displays
// alerts as they're generated." Everything downstream of the FIX session
// (ring buffer, LivePipeline, all six detectors, DbAlertSink) is exactly
// Phase 6/7/8's already-tested wiring, reused unmodified -- see
// cpp/tests/fix/fix_to_pipeline_integration_test.cpp, which this file's
// FIX-session setup and simulator-to-FIX-message translation are directly
// adapted from. What's new here: a *production* (non-gtest) FIX loopback
// session bootstrap, a paced/looping market-data feeder (real tests fire a
// whole batch at once; a demo server needs to keep producing new data for
// as long as it runs), LiveBookRegistry (Phase 9's new thread-safety layer
// for cross-thread book reads), and the Crow HTTP server itself.
#include <quickfix/Dictionary.h>
#include <quickfix/Log.h>
#include <quickfix/MessageStore.h>
#include <quickfix/Session.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketAcceptor.h>
#include <quickfix/SocketInitiator.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unordered_map>

#include <crow.h>

#include "account_registry.hpp"
#include "alert_store.hpp"
#include "api_server.hpp"
#include "db_alert_sink.hpp"
#include "fix_application.hpp"
#include "front_running_detector.hpp"
#include "live_book_registry.hpp"
#include "live_pipeline.hpp"
#include "marking_the_close_detector.hpp"
#include "message_translator.hpp"
#include "ml_anomaly_detector.hpp"
#include "ml_score_client.hpp"
#include "ml_scoring_worker.hpp"
#include "pipeline_event_sink.hpp"
#include "simulator.hpp"
#include "spoofing_layering_detector.hpp"
#include "statistical_baseline_detector.hpp"
#include "wash_trade_detector.hpp"

#ifndef TSE_API_QUICKFIX_FIX42_SPEC
#error "TSE_API_QUICKFIX_FIX42_SPEC must be defined by CMake -- see cpp/api/CMakeLists.txt"
#endif

using tse::fix::SurveillanceFixApplication;

namespace {

tse::fix::Order to_fix_order(const tse::simulator::Order& order) {
    tse::fix::Order out;
    out.order_id = order.order_id;
    out.account_id = order.account_id;
    out.instrument_id = order.instrument_id;
    out.side = order.side == tse::simulator::Side::kBuy ? tse::fix::Side::kBuy : tse::fix::Side::kSell;
    out.price = order.price;
    out.qty = order.qty;
    out.order_type = order.order_type == tse::simulator::OrderType::kMarket ? tse::fix::OrderType::kMarket
                                                                             : tse::fix::OrderType::kLimit;
    out.timestamp_ns = order.timestamp_ns;
    out.status = order.status == tse::simulator::OrderStatus::kCancelled ? tse::fix::OrderStatus::kCancelled
                                                                          : tse::fix::OrderStatus::kNew;
    out.venue = order.venue;
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

tse::detectors::Entity to_entity(const tse::simulator::Account& account) {
    tse::detectors::Entity entity;
    entity.account_id = account.account_id;
    entity.beneficial_owner_id = account.beneficial_owner_id;
    entity.entity_type = tse::simulator::to_string(account.entity_type);
    entity.linked_account_ids = account.linked_account_ids;
    return entity;
}

int64_t now_epoch_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// session_start_ns is deliberately a real epoch-nanosecond anchor, not the
// SimulatorConfig default of 0 -- CLAUDE.md's own style rule is "all
// timestamps ... stored as int64_t epoch nanos", and every downstream
// consumer of Alert::window_start_ns/window_end_ns (the dashboard's
// `new Date(ns / 1e6)`, in particular) assumes that convention holds.
// Passing 0 silently produced alerts timestamped at the Unix epoch, which
// the dashboard correctly rendered as "12/31/1969" -- a real bug, not a
// display artifact, and not something a detector could have caused: every
// detector only ever passes through fix::Order/Execution.timestamp_ns
// values verbatim.
tse::simulator::SimulatorConfig demo_session_config(uint64_t seed, int64_t session_start_ns) {
    tse::simulator::SimulatorConfig config;
    config.random_seed = seed;
    config.session_start_ns = session_start_ns;
    config.session_duration_ns = 90'000'000'000LL;  // 90 synthetic seconds -- one demo "loop"
    config.baseline_orders_per_second = 6.0;
    config.num_equity_instruments = 2;
    config.num_fx_instruments = 1;
    config.num_fixed_income_instruments = 0;
    config.num_independent_accounts = 12;
    config.num_linked_account_pairs = 3;
    config.wash_trade = {1, 0.6};
    config.spoofing_layering = {1, 0.6};
    config.marking_the_close = {1, 0.6};
    config.front_running = {1, 0.6};
    return config;
}

std::vector<std::unique_ptr<tse::detectors::IDetector>> make_six_detectors(
    const tse::simulator::SimulationOutput& simulation, tse::ml_client::MlScoringWorker* ml_worker) {
    std::vector<std::unique_ptr<tse::detectors::IDetector>> result;
    result.push_back(std::make_unique<tse::detectors::WashTradeDetector>());
    result.push_back(std::make_unique<tse::detectors::SpoofingLayeringDetector>());
    tse::detectors::MarkingTheCloseConfig mtc_config;
    for (const auto& instrument : simulation.instruments) {
        mtc_config.close_time_ns_by_instrument[instrument.instrument_id] = instrument.session_close_ns;
    }
    result.push_back(std::make_unique<tse::detectors::MarkingTheCloseDetector>(mtc_config));
    result.push_back(std::make_unique<tse::detectors::FrontRunningDetector>());
    result.push_back(std::make_unique<tse::detectors::StatisticalBaselineDetector>());
    result.push_back(std::make_unique<tse::ml_client::MlAnomalyDetector>(ml_worker));
    return result;
}

// Sends one demo session's worth of simulator output over a real FIX
// session, paced with a small sleep between messages -- unlike the tests
// this is adapted from (which fire a whole batch as fast as possible to
// prove throughput), a demo server wants visible, gradual arrival for
// whatever's watching the dashboard.
//
// session_start_ns is the SAME fixed anchor every loop iteration uses
// (captured once in main(), not a fresh now() per call) -- deliberately,
// not advancing: MarkingTheCloseDetector's config is built once, from one
// session_close_ns value, at process startup (see make_six_detectors());
// if each loop advanced to a genuinely later real time, that fixed close
// time would fall further into the past every iteration and the detector
// would stop firing after the first loop. A single shared anchor keeps
// every loop's synthetic session landing in the same real time window --
// real, valid epoch nanoseconds (fixing the "12/31/1969" display bug)
// without disturbing a detector whose config isn't designed to be
// rebuilt per session.
void feed_one_session(const FIX::SessionID& initiator_session_id, uint64_t seed, int64_t session_start_ns) {
    tse::simulator::SimulationOutput simulation =
        tse::simulator::generate_simulation(demo_session_config(seed, session_start_ns));

    std::unordered_map<std::string, tse::fix::Side> side_by_order_id;
    for (const auto& order : simulation.orders) {
        side_by_order_id[order.order_id] = order.side == tse::simulator::Side::kBuy ? tse::fix::Side::kBuy
                                                                                     : tse::fix::Side::kSell;
    }

    // Orders and executions are sent interleaved in their own recorded
    // time order (simulator's output is time-sorted separately per type;
    // merging by original timestamp keeps New-before-Execution causality
    // intact for every order) rather than "all orders, then all
    // executions" -- closer to what a real market actually looks like on
    // the wire, and what the dashboard's live ticker should show.
    struct Item {
        int64_t timestamp_ns;
        bool is_execution;
        size_t index;
    };
    std::vector<Item> items;
    items.reserve(simulation.orders.size() + simulation.executions.size());
    for (size_t i = 0; i < simulation.orders.size(); ++i) {
        items.push_back({simulation.orders[i].timestamp_ns, false, i});
    }
    for (size_t i = 0; i < simulation.executions.size(); ++i) {
        items.push_back({simulation.executions[i].timestamp_ns, true, i});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.timestamp_ns < b.timestamp_ns; });

    for (const Item& item : items) {
        if (!item.is_execution) {
            const tse::simulator::Order& order = simulation.orders[item.index];
            tse::fix::Order fix_order = to_fix_order(order);
            if (order.status == tse::simulator::OrderStatus::kNew) {
                auto message = tse::fix::to_new_order_single(fix_order);
                FIX::Session::sendToTarget(message, initiator_session_id);
            } else if (order.status == tse::simulator::OrderStatus::kCancelled) {
                auto message = tse::fix::to_order_cancel_request(fix_order);
                FIX::Session::sendToTarget(message, initiator_session_id);
            }
        } else {
            const tse::simulator::Execution& execution = simulation.executions[item.index];
            tse::fix::Side side = side_by_order_id.at(execution.order_id);
            auto message = tse::fix::to_execution_report(to_fix_execution(execution, side));
            FIX::Session::sendToTarget(message, initiator_session_id);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    std::cerr << "[demo-feed] session seed=" << seed << " sent " << items.size() << " messages across "
              << simulation.instruments.size() << " instruments\n";
}

}  // namespace

int main(int argc, char** argv) {
    int http_port = 8081;
    if (argc > 1) http_port = std::atoi(argv[1]);

    // Captured once, shared by every simulated session for this process's
    // entire lifetime -- see feed_one_session()'s own comment for why this
    // is a single fixed anchor, not a fresh now() per loop.
    const int64_t demo_epoch_anchor_ns = now_epoch_ns();

    // --- AlertStore / DbAlertSink -------------------------------------------
    tse::db::AlertStore store;
    store.apply_schema();
    tse::db::DbAlertSink alert_sink(&store);

    // --- ML scoring worker (Phase 7) ----------------------------------------
    // Fails open if ml_service/ isn't running (see cpp/ml_client/README.md)
    // -- safe to wire in unconditionally.
    tse::ml_client::MlScoringWorker ml_worker(tse::ml_client::MlScoreClient({}), &alert_sink);
    std::atomic<bool> ml_worker_stop{false};
    std::thread ml_worker_thread([&] { ml_worker.run(ml_worker_stop); });

    // --- A representative account/instrument universe for the pipeline's
    // AccountRegistry, built once from a throwaway session generation (the
    // account set is what MarkingTheCloseDetector's config and
    // AccountRegistry both need up front; feed_one_session() below
    // regenerates fresh orders/executions against this same account set
    // every loop via a fixed instrument/account *count*, even though the
    // actual accounts objects differ seed to seed -- see the account
    // re-registration note below).
    tse::simulator::SimulationOutput seed_simulation =
        tse::simulator::generate_simulation(demo_session_config(/*seed=*/1, demo_epoch_anchor_ns));
    tse::detectors::AccountRegistry accounts;
    for (const auto& account : seed_simulation.accounts) accounts.add(to_entity(account));

    tse::pipeline::LivePipeline live_pipeline(make_six_detectors(seed_simulation, &ml_worker), std::move(accounts));
    tse::ingestion::SpscRingBuffer<tse::ingestion::IngestionEvent> queue(8192);
    tse::api::LiveBookRegistry book_registry(queue, live_pipeline, &alert_sink);
    tse::pipeline::PipelineEventSink event_sink(queue, /*kafka_producer=*/nullptr);

    std::atomic<bool> book_consumer_done{false};
    std::thread book_consumer_thread([&] { book_registry.run(book_consumer_done); });

    // --- Real FIX 4.2 loopback session (production version of
    // cpp/tests/fix/fix_session_test_fixture.hpp's setup) ---------------------
    SurveillanceFixApplication acceptor_app;
    SurveillanceFixApplication initiator_app;
    acceptor_app.set_event_sink(&event_sink);

    FIX::SessionID acceptor_session_id("FIX.4.2", "ACCEPTOR", "INITIATOR");
    FIX::SessionID initiator_session_id("FIX.4.2", "INITIATOR", "ACCEPTOR");

    FIX::Dictionary common;
    common.setString("StartTime", "00:00:00");
    common.setString("EndTime", "00:00:00");
    common.setString("HeartBtInt", "30");
    common.setString("ReconnectInterval", "2");
    common.setString("DataDictionary", TSE_API_QUICKFIX_FIX42_SPEC);

    FIX::Dictionary acceptor_dict = common;
    acceptor_dict.setString("ConnectionType", "acceptor");
    acceptor_dict.setInt("SocketAcceptPort", 19999);

    FIX::Dictionary initiator_dict = common;
    initiator_dict.setString("ConnectionType", "initiator");
    initiator_dict.setString("SocketConnectHost", "127.0.0.1");
    initiator_dict.setInt("SocketConnectPort", 19999);

    FIX::SessionSettings acceptor_settings;
    acceptor_settings.set(common);
    acceptor_settings.set(acceptor_session_id, acceptor_dict);
    FIX::SessionSettings initiator_settings;
    initiator_settings.set(common);
    initiator_settings.set(initiator_session_id, initiator_dict);

    FIX::MemoryStoreFactory acceptor_store_factory;
    FIX::MemoryStoreFactory initiator_store_factory;
    FIX::ScreenLogFactory acceptor_log_factory(false, false, false);
    FIX::ScreenLogFactory initiator_log_factory(false, false, false);

    FIX::SocketAcceptor acceptor(acceptor_app, acceptor_store_factory, acceptor_settings, acceptor_log_factory);
    FIX::SocketInitiator initiator(initiator_app, initiator_store_factory, initiator_settings, initiator_log_factory);
    acceptor.start();
    initiator.start();

    if (!acceptor_app.wait_for_logon(std::chrono::seconds(10)) ||
        !initiator_app.wait_for_logon(std::chrono::seconds(10))) {
        std::cerr << "[demo-server] FIX loopback session failed to logon within 10s\n";
        return 1;
    }
    std::cerr << "[demo-server] FIX loopback session up\n";

    // --- Market-data feeder thread: loops forever, one demo session at a
    // time, so the dashboard always has something new to observe for as
    // long as this process runs.
    std::thread feeder_thread([&] {
        uint64_t seed = 2;
        while (true) {
            feed_one_session(initiator_session_id, seed++, demo_epoch_anchor_ns);
        }
    });

    // --- REST API (Crow) -----------------------------------------------------
    tse::api::App app;
    tse::api::register_routes(app, &store, &book_registry);
    std::cerr << "[demo-server] listening on http://127.0.0.1:" << http_port << "\n";
    app.port(http_port).multithreaded().run();  // blocks for the process's lifetime

    // Unreachable under normal operation (app.run() only returns on
    // shutdown, which this demo server has no signal handler for) -- left
    // in for symmetry/documentation of what teardown would look like.
    feeder_thread.detach();
    book_consumer_done.store(true, std::memory_order_release);
    book_consumer_thread.join();
    ml_worker_stop.store(true, std::memory_order_release);
    ml_worker_thread.join();
    initiator.stop();
    acceptor.stop();
    return 0;
}
