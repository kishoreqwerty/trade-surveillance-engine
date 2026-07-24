#include "replay_runner.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include "account_registry.hpp"
#include "alert_sink.hpp"
#include "front_running_detector.hpp"
#include "i_detector.hpp"
#include "ingestion_event.hpp"
#include "kafka_consumer.hpp"
#include "kafka_producer.hpp"
#include "live_consumer.hpp"
#include "live_pipeline.hpp"
#include "marking_the_close_detector.hpp"
#include "ml_anomaly_detector.hpp"
#include "ml_score_client.hpp"
#include "ml_scoring_worker.hpp"
#include "spoofing_layering_detector.hpp"
#include "spsc_ring_buffer.hpp"
#include "statistical_baseline_detector.hpp"
#include "wash_trade_detector.hpp"

namespace tse::harness {

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

// Same interleave-by-original-timestamp merge cpp/api/main.cpp's
// feed_one_session() uses, so New-before-Cancel/New-before-Execution
// causality is preserved on the wire exactly as it would be for a real FIX
// session -- Kafka/the ring buffer only ever see events in this order.
std::vector<tse::ingestion::IngestionEvent> to_ingestion_events(const tse::simulator::SimulationOutput& simulation) {
    std::unordered_map<std::string, tse::fix::Side> side_by_order_id;
    for (const auto& order : simulation.orders) {
        side_by_order_id[order.order_id] =
            order.side == tse::simulator::Side::kBuy ? tse::fix::Side::kBuy : tse::fix::Side::kSell;
    }

    struct Item {
        int64_t timestamp_ns;
        bool is_execution;
        size_t index;
    };
    std::vector<Item> items;
    items.reserve(simulation.orders.size() + simulation.executions.size());
    for (size_t i = 0; i < simulation.orders.size(); ++i) items.push_back({simulation.orders[i].timestamp_ns, false, i});
    for (size_t i = 0; i < simulation.executions.size(); ++i) {
        items.push_back({simulation.executions[i].timestamp_ns, true, i});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.timestamp_ns < b.timestamp_ns; });

    std::vector<tse::ingestion::IngestionEvent> events;
    events.reserve(items.size());
    for (const Item& item : items) {
        if (!item.is_execution) {
            events.push_back(to_fix_order(simulation.orders[item.index]));
        } else {
            const tse::simulator::Execution& execution = simulation.executions[item.index];
            tse::fix::Side side = side_by_order_id.at(execution.order_id);
            events.push_back(to_fix_execution(execution, side));
        }
    }
    return events;
}

std::vector<std::unique_ptr<tse::detectors::IDetector>> make_five_detectors(
    const tse::simulator::SimulationOutput& simulation) {
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
    return result;
}

// Smallest power of two >= n, floored at 1024 -- generous enough that a
// replay never has to invoke the ring buffer's drop-oldest backpressure
// path (see spsc_ring_buffer.hpp); replay_through_kafka asserts
// dropped_count() == 0 afterward specifically because silently dropping an
// event here would corrupt this phase's precision/recall numbers with no
// visible signal.
std::size_t ring_buffer_capacity_for(std::size_t n) {
    std::size_t capacity = 1024;
    while (capacity < n) capacity *= 2;
    return capacity;
}

}  // namespace

ReplayResult replay_through_kafka(const tse::simulator::SimulationOutput& simulation, const std::string& brokers,
                                   const std::string& topic, int publish_timeout_ms, int poll_timeout_ms,
                                   const MlEvalConfig* ml_eval) {
    std::vector<tse::ingestion::IngestionEvent> events = to_ingestion_events(simulation);

    {
        tse::ingestion::KafkaProducer producer(brokers, topic);
        for (const auto& event : events) {
            while (!producer.publish(event)) {
                producer.poll(10);
            }
            producer.poll(0);
        }
        if (!producer.flush(publish_timeout_ms)) {
            throw std::runtime_error("replay_through_kafka: Kafka publish did not flush within " +
                                      std::to_string(publish_timeout_ms) + "ms -- is the broker at " + brokers +
                                      " reachable? (docker compose up -d kafka)");
        }
    }

    tse::detectors::AccountRegistry accounts;
    for (const auto& account : simulation.accounts) accounts.add(to_entity(account));

    tse::pipeline::CollectingAlertSink alert_sink;

    // Constructed before the pipeline/detectors below since MlAnomalyDetector
    // (when ml_eval is set) holds a raw pointer to it -- see MlEvalConfig's
    // header comment for the health-check-before-replay and
    // alert_threshold=0.0 reasoning.
    std::optional<tse::ml_client::MlScoringWorker> ml_worker;
    if (ml_eval != nullptr) {
        tse::ml_client::MlScoreClient client(ml_eval->client);
        if (!client.health_check()) {
            throw std::runtime_error("replay_through_kafka: ml_service health check failed at " +
                                      ml_eval->client.base_url +
                                      " -- start ml_service before running an ML-inclusive evaluation");
        }
        tse::ml_client::MlScoringWorkerConfig worker_config;
        worker_config.alert_threshold = 0.0;
        worker_config.queue_capacity = ring_buffer_capacity_for(events.size());
        ml_worker.emplace(std::move(client), &alert_sink, worker_config);
    }

    std::vector<std::unique_ptr<tse::detectors::IDetector>> detectors = make_five_detectors(simulation);
    if (ml_worker.has_value()) {
        detectors.push_back(std::make_unique<tse::ml_client::MlAnomalyDetector>(&ml_worker.value()));
    }
    tse::pipeline::LivePipeline pipeline(std::move(detectors), std::move(accounts));

    tse::ingestion::SpscRingBuffer<tse::ingestion::IngestionEvent> queue(ring_buffer_capacity_for(events.size()));
    tse::pipeline::LiveConsumer consumer(queue, pipeline, &alert_sink);

    std::atomic<bool> producer_done{false};
    std::thread consumer_thread([&] { consumer.run(producer_done); });

    std::atomic<bool> ml_stop{false};
    std::optional<std::thread> ml_worker_thread;
    if (ml_worker.has_value()) {
        ml_worker_thread.emplace([&] { ml_worker->run(ml_stop); });
    }

    tse::ingestion::KafkaReplayConsumer replay_consumer(brokers, topic);
    replay_consumer.seek_to_beginning();
    uint64_t replayed = 0;
    int consecutive_empty = 0;
    while (replayed < events.size() && consecutive_empty < 10) {
        auto event = replay_consumer.poll(poll_timeout_ms);
        if (event.has_value()) {
            queue.push(std::move(*event));
            ++replayed;
            consecutive_empty = 0;
        } else {
            ++consecutive_empty;
        }
    }

    producer_done.store(true, std::memory_order_release);
    consumer_thread.join();

    // Only safe to signal the ML worker to stop *after* the consumer thread
    // -- the only caller of MlAnomalyDetector::evaluate()/submit() -- has
    // fully finished; run()'s drain contract (ml_scoring_worker.hpp) then
    // guarantees every request submitted before this point is fully scored,
    // with any resulting Alert already in alert_sink, before join() returns.
    // Both threads are joined before any of the throwing checks below run,
    // so no std::thread is ever destroyed while still joinable.
    if (ml_worker_thread.has_value()) {
        ml_stop.store(true, std::memory_order_release);
        ml_worker_thread->join();
        if (ml_worker->requests_dropped() != 0) {
            throw std::runtime_error("replay_through_kafka: ML scoring queue dropped " +
                                      std::to_string(ml_worker->requests_dropped()) +
                                      " request(s) under backpressure -- queue_capacity too small for this replay");
        }
    }

    if (replayed != events.size()) {
        throw std::runtime_error("replay_through_kafka: only replayed " + std::to_string(replayed) + "/" +
                                  std::to_string(events.size()) +
                                  " events back from Kafka -- broker issue or topic collision?");
    }
    if (queue.dropped_count() != 0) {
        throw std::runtime_error("replay_through_kafka: ring buffer dropped " +
                                  std::to_string(queue.dropped_count()) +
                                  " event(s) under drop-oldest backpressure -- capacity too small for this replay");
    }

    ReplayResult result;
    result.alerts = alert_sink.alerts();
    result.events_total = events.size();
    result.events_replayed_from_kafka = replayed;
    result.events_processed = consumer.events_processed();
    result.events_skipped_inconsistent = consumer.events_skipped_inconsistent();
    result.ring_buffer_dropped = queue.dropped_count();
    return result;
}

}  // namespace tse::harness
