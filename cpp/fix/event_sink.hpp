#pragma once

#include "types.hpp"

namespace tse::fix {

// Optional hot-path hook: SurveillanceFixApplication calls this for every
// successfully parsed Order/Execution, in addition to (not instead of) its
// own test-oriented accumulation (received_orders()/received_executions()).
//
// fix/ owns this abstraction rather than depending on ingestion/ directly —
// Phase 6's cpp/pipeline/ module provides the concrete implementation that
// bridges into the SPSC ring buffer (and Kafka), keeping the architecture
// doc's dependency direction intact (ingestion/ depends on fix/, never the
// reverse; a live-wiring module depending on fix/ is fine, fix/ depending
// on a live-wiring module would not be).
class IEventSink {
public:
    virtual ~IEventSink() = default;
    virtual void on_order(const Order& order) = 0;
    virtual void on_execution(const Execution& execution) = 0;
};

}  // namespace tse::fix
