#pragma once

#include <memory>
#include <string>

#include "ingestion_event.hpp"

namespace RdKafka {
class Conf;
class Producer;
}  // namespace RdKafka

namespace tse::ingestion {

// The durable/replayable layer behind the SPSC ring buffer (see
// spsc_ring_buffer.hpp's header comment and cpp/ingestion/README.md). Wraps
// the real librdkafka C++ Producer — not a hand-rolled protocol client.
//
// publish() is asynchronous: librdkafka's produce() only enqueues the
// message locally and returns immediately; the actual network send happens
// on librdkafka's own background I/O thread. This is what the architecture
// doc means by "Kafka (durability, async)" — nothing here blocks the
// ingestion hot path.
class KafkaProducer {
public:
    // brokers: e.g. "localhost:9092". Throws std::runtime_error if the
    // producer handle can't be created (invalid config) — construction is
    // not the place to silently degrade; callers decide fallback behavior.
    KafkaProducer(const std::string& brokers, std::string topic);
    ~KafkaProducer();

    KafkaProducer(const KafkaProducer&) = delete;
    KafkaProducer& operator=(const KafkaProducer&) = delete;

    // Encodes and enqueues `event`. Non-blocking. Returns false if
    // librdkafka's local outbound queue was full (queue.buffering.max.messages)
    // — the caller can poll()/retry, this does not throw for a transient
    // backpressure condition.
    bool publish(const IngestionEvent& event);

    // Drains delivery-report callbacks and other internal librdkafka
    // events. Must be called periodically (or the outbound queue can fill
    // up); safe to call with timeout_ms=0 for a non-blocking drain.
    void poll(int timeout_ms = 0);

    // Blocks until every message produced so far has been acknowledged by
    // the broker or timeout_ms elapses. Used at test/shutdown boundaries —
    // "durable" is only provable after a flush, never assumed right after
    // publish() returns.
    bool flush(int timeout_ms);

private:
    std::unique_ptr<RdKafka::Conf> conf_;
    std::unique_ptr<RdKafka::Producer> producer_;
    std::string topic_;
};

}  // namespace tse::ingestion
