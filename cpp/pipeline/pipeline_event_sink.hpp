#pragma once

#include "event_sink.hpp"
#include "ingestion_event.hpp"
#include "kafka_producer.hpp"
#include "spsc_ring_buffer.hpp"

namespace tse::pipeline {

// The concrete tse::fix::IEventSink this phase provides: every Order or
// Execution SurveillanceFixApplication parses off the wire is pushed onto
// the SPSC ring buffer (hot path) and, if a KafkaProducer was supplied,
// published to Kafka (durability, async) too -- both fed at the point of
// arrival, independently, exactly as the architecture doc's "ring buffer
// (hot path) + Kafka (durability, async)" pairing intends. Publishing at
// arrival rather than after the ring buffer pop matters: Kafka's job is to
// be the complete durable record regardless of what the lossy,
// drop-oldest ring buffer had to discard under backpressure (see
// cpp/ingestion/README.md) -- feeding it only from the consumer side would
// let it silently share the ring buffer's drops instead of being the
// thing that survives them.
//
// kafka_producer is nullable: a live broker isn't available in every
// environment this runs in (see cpp/ingestion/kafka_replay_test.cpp's own
// GTEST_SKIP precedent), and the ring buffer -> book -> detectors hot path
// this phase's sustained-load test stresses doesn't depend on Kafka being
// present.
class PipelineEventSink : public tse::fix::IEventSink {
public:
    PipelineEventSink(tse::ingestion::SpscRingBuffer<tse::ingestion::IngestionEvent>& queue,
                       tse::ingestion::KafkaProducer* kafka_producer)
        : queue_(queue), kafka_producer_(kafka_producer) {}

    void on_order(const tse::fix::Order& order) override { publish(tse::ingestion::IngestionEvent{order}); }

    void on_execution(const tse::fix::Execution& execution) override {
        publish(tse::ingestion::IngestionEvent{execution});
    }

private:
    void publish(tse::ingestion::IngestionEvent event) {
        if (kafka_producer_ != nullptr) {
            kafka_producer_->publish(event);
        }
        queue_.push(std::move(event));
    }

    tse::ingestion::SpscRingBuffer<tse::ingestion::IngestionEvent>& queue_;
    tse::ingestion::KafkaProducer* kafka_producer_;
};

}  // namespace tse::pipeline
