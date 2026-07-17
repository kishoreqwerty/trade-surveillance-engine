#include "kafka_producer.hpp"

#include <rdkafkacpp.h>

#include <stdexcept>

#include "event_codec.hpp"

namespace tse::ingestion {

KafkaProducer::KafkaProducer(const std::string& brokers, std::string topic) : topic_(std::move(topic)) {
    std::string errstr;

    conf_.reset(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    if (conf_->set("bootstrap.servers", brokers, errstr) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("KafkaProducer: failed to set bootstrap.servers: " + errstr);
    }

    producer_.reset(RdKafka::Producer::create(conf_.get(), errstr));
    if (!producer_) {
        throw std::runtime_error("KafkaProducer: failed to create producer: " + errstr);
    }
}

KafkaProducer::~KafkaProducer() {
    if (producer_) {
        producer_->flush(5000);
    }
}

bool KafkaProducer::publish(const IngestionEvent& event) {
    std::string payload = encode(event);

    RdKafka::ErrorCode err =
        producer_->produce(topic_, RdKafka::Topic::PARTITION_UA, RdKafka::Producer::RK_MSG_COPY,
                            const_cast<char*>(payload.data()), payload.size(), nullptr, 0, 0, nullptr);

    if (err == RdKafka::ERR__QUEUE_FULL) {
        return false;
    }
    if (err != RdKafka::ERR_NO_ERROR) {
        throw std::runtime_error("KafkaProducer: produce() failed: " + RdKafka::err2str(err));
    }
    return true;
}

void KafkaProducer::poll(int timeout_ms) { producer_->poll(timeout_ms); }

bool KafkaProducer::flush(int timeout_ms) { return producer_->flush(timeout_ms) == RdKafka::ERR_NO_ERROR; }

}  // namespace tse::ingestion
