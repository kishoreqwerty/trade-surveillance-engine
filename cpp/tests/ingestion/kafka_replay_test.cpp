#include <gtest/gtest.h>

#include <chrono>
#include <sstream>
#include <vector>

#include "event_codec.hpp"
#include "kafka_consumer.hpp"
#include "kafka_producer.hpp"

using namespace tse::fix;
using namespace tse::ingestion;

namespace {

constexpr const char* kBrokers = "localhost:9092";

// A deterministic recorded sequence mixing Orders and Executions — this is
// the "recorded message sequence" the done-when criterion asks to replay.
std::vector<IngestionEvent> make_recorded_sequence() {
    std::vector<IngestionEvent> events;
    for (int i = 0; i < 25; ++i) {
        Order order;
        order.order_id = "ORD-" + std::to_string(i);
        order.account_id = "ACC-" + std::to_string(i % 5);
        order.instrument_id = "ACME";
        order.side = (i % 2 == 0) ? Side::kBuy : Side::kSell;
        order.price = 100.0 + static_cast<double>(i) * 0.01;
        order.qty = 100 + i;
        order.order_type = OrderType::kLimit;
        order.timestamp_ns = 1'700'000'000'000'000'000LL + i;
        order.status = OrderStatus::kNew;
        order.venue = "SIM";
        events.push_back(order);

        Execution execution;
        execution.trade_id = "EXE-" + std::to_string(i);
        execution.order_id = order.order_id;
        execution.account_id = order.account_id;
        execution.instrument_id = order.instrument_id;
        execution.side = order.side;
        execution.price = order.price;
        execution.qty = order.qty;
        execution.timestamp_ns = order.timestamp_ns + 1;
        execution.counterparty_account_id = "ACC-COUNTERPARTY";
        execution.venue = "SIM";
        events.push_back(execution);
    }
    return events;
}

// "Downstream state" stand-in: concatenates every event's exact encoded
// bytes in arrival order. Two runs producing identical concatenated output
// is what "bit-identical downstream state" means here — not a hash (which
// would only prove "probably identical"), the literal bytes.
std::string downstream_state(const std::vector<IngestionEvent>& events) {
    std::ostringstream oss;
    for (const auto& event : events) oss << encode(event) << "\n";
    return oss.str();
}

std::string topic_name() {
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return "tse-ingestion-replay-test-" + std::to_string(now);
}

}  // namespace

// Requires a real Kafka broker (docker compose up -d kafka). Skips cleanly
// — does not fail — if one isn't reachable, since this is an integration
// test against an external service, not a pure unit test.
TEST(KafkaReplay, ReplayedSequenceReproducesBitIdenticalDownstreamState) {
    std::vector<IngestionEvent> recorded = make_recorded_sequence();
    std::string reference_state = downstream_state(recorded);
    std::string topic = topic_name();

    {
        KafkaProducer producer(kBrokers, topic);
        for (const auto& event : recorded) {
            ASSERT_TRUE(producer.publish(event));
        }
        if (!producer.flush(10000)) {
            GTEST_SKIP() << "Kafka broker at " << kBrokers
                         << " not reachable within 10s — run `docker compose up -d kafka` first.";
        }
    }

    // Two independent replays of the same recorded topic, from two
    // independent consumer instances, each starting fresh from the
    // beginning — this is the actual determinism claim: not just "replay
    // matches the original" but "replay is itself reproducible."
    auto replay_once = [&]() {
        KafkaReplayConsumer consumer(kBrokers, topic);
        std::vector<IngestionEvent> replayed;
        int consecutive_empty = 0;
        while (replayed.size() < recorded.size() && consecutive_empty < 5) {
            auto event = consumer.poll(2000);
            if (event.has_value()) {
                replayed.push_back(*event);
                consecutive_empty = 0;
            } else {
                ++consecutive_empty;
            }
        }
        return replayed;
    };

    std::vector<IngestionEvent> replay_a = replay_once();
    std::vector<IngestionEvent> replay_b = replay_once();

    ASSERT_EQ(replay_a.size(), recorded.size())
        << "first replay did not recover the full recorded sequence";
    ASSERT_EQ(replay_b.size(), recorded.size())
        << "second replay did not recover the full recorded sequence";

    std::string replay_a_state = downstream_state(replay_a);
    std::string replay_b_state = downstream_state(replay_b);

    EXPECT_EQ(replay_a_state, reference_state)
        << "first replay's downstream state diverged from the original in-memory sequence";
    EXPECT_EQ(replay_b_state, reference_state)
        << "second replay's downstream state diverged from the original in-memory sequence";
    EXPECT_EQ(replay_a_state, replay_b_state) << "two replays of the same topic produced different state";
}
