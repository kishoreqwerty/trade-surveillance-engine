#pragma once

#include <variant>

#include "types.hpp"  // tse::fix::Order, tse::fix::Execution

namespace tse::ingestion {

// What actually flows through the ring buffer and Kafka in the live
// pipeline: a single parsed FIX message is either an Order (from
// NewOrderSingle/OrderCancelRequest) or an Execution (from
// ExecutionReport) — never both — but both need to travel through the same
// queue, in arrival order, since the order book applies them in sequence.
using IngestionEvent = std::variant<tse::fix::Order, tse::fix::Execution>;

inline bool is_order(const IngestionEvent& event) {
    return std::holds_alternative<tse::fix::Order>(event);
}

inline bool is_execution(const IngestionEvent& event) {
    return std::holds_alternative<tse::fix::Execution>(event);
}

}  // namespace tse::ingestion
