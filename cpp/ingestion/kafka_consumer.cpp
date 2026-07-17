#include "kafka_consumer.hpp"

#include <rdkafkacpp.h>

#include <stdexcept>
#include <vector>

#include "event_codec.hpp"

namespace tse::ingestion {

KafkaReplayConsumer::KafkaReplayConsumer(const std::string& brokers, std::string topic, int32_t partition)
    : topic_(std::move(topic)), partition_(partition) {
    std::string errstr;

    conf_.reset(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    if (conf_->set("bootstrap.servers", brokers, errstr) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("KafkaReplayConsumer: failed to set bootstrap.servers: " + errstr);
    }
    // Required by KafkaConsumer::create() even though we use explicit
    // assign()/seek() rather than group-based subscribe() — the group ID
    // is never actually used for offset coordination here.
    if (conf_->set("group.id", "tse-replay-consumer", errstr) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("KafkaReplayConsumer: failed to set group.id: " + errstr);
    }
    if (conf_->set("enable.partition.eof", "true", errstr) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("KafkaReplayConsumer: failed to set enable.partition.eof: " + errstr);
    }

    consumer_.reset(RdKafka::KafkaConsumer::create(conf_.get(), errstr));
    if (!consumer_) {
        throw std::runtime_error("KafkaReplayConsumer: failed to create consumer: " + errstr);
    }

    seek_to_beginning();
}

KafkaReplayConsumer::~KafkaReplayConsumer() {
    if (consumer_) {
        consumer_->close();
    }
}

void KafkaReplayConsumer::seek_to_beginning() {
    std::unique_ptr<RdKafka::TopicPartition> tp(
        RdKafka::TopicPartition::create(topic_, partition_, RdKafka::Topic::OFFSET_BEGINNING));
    std::vector<RdKafka::TopicPartition*> partitions{tp.get()};

    RdKafka::ErrorCode err = consumer_->assign(partitions);
    if (err != RdKafka::ERR_NO_ERROR) {
        throw std::runtime_error("KafkaReplayConsumer: assign() failed: " + RdKafka::err2str(err));
    }
}

std::optional<IngestionEvent> KafkaReplayConsumer::poll(int timeout_ms) {
    std::unique_ptr<RdKafka::Message> msg(consumer_->consume(timeout_ms));

    switch (msg->err()) {
        case RdKafka::ERR_NO_ERROR: {
            std::string payload(static_cast<const char*>(msg->payload()), msg->len());
            return decode(payload);
        }
        case RdKafka::ERR__TIMED_OUT:
        case RdKafka::ERR__PARTITION_EOF:
            return std::nullopt;
        default:
            throw std::runtime_error("KafkaReplayConsumer: consume() failed: " + msg->errstr());
    }
}

}  // namespace tse::ingestion
