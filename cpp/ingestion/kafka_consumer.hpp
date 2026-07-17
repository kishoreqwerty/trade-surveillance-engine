#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "ingestion_event.hpp"

namespace RdKafka {
class Conf;
class KafkaConsumer;
}  // namespace RdKafka

namespace tse::ingestion {

// Reads a topic back from the beginning — true replay semantics via
// explicit partition assignment + OFFSET_BEGINNING, deliberately not
// consumer-group subscription (subscribe() + group offset tracking would
// make "replay from the start" depend on whether this group has consumed
// from this topic before, which is exactly the nondeterminism Phase 10's
// evaluation harness cannot tolerate — replay must start from the same
// place every single time, regardless of history).
class KafkaReplayConsumer {
public:
    KafkaReplayConsumer(const std::string& brokers, std::string topic, int32_t partition = 0);
    ~KafkaReplayConsumer();

    KafkaReplayConsumer(const KafkaReplayConsumer&) = delete;
    KafkaReplayConsumer& operator=(const KafkaReplayConsumer&) = delete;

    // Seeks to the first message ever produced to this topic/partition.
    // Call before the first poll() (or again to replay a second time).
    void seek_to_beginning();

    // Waits up to timeout_ms for the next message. std::nullopt on timeout
    // or on reaching the end of what's currently in the partition (i.e.
    // replay complete) — both are normal, expected outcomes, not errors.
    std::optional<IngestionEvent> poll(int timeout_ms);

private:
    std::unique_ptr<RdKafka::Conf> conf_;
    std::unique_ptr<RdKafka::KafkaConsumer> consumer_;
    std::string topic_;
    int32_t partition_;
};

}  // namespace tse::ingestion
