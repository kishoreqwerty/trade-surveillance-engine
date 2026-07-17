#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "alert_sink.hpp"
#include "ingestion_event.hpp"
#include "live_pipeline.hpp"
#include "spsc_ring_buffer.hpp"

namespace tse::pipeline {

struct EventLatencySample {
    int64_t book_apply_ns{0};
    int64_t detectors_ns{0};
};

// Owns the ring-buffer pop loop: the actual "consumer" side of the
// producer/consumer relationship Phase 3's SpscRingBuffer was built for,
// now popping into a real LivePipeline instead of Phase 3's own
// two_thread_pipeline_test.cpp stand-in counter. Thread-agnostic by
// design — run() is a plain method a caller invokes on whatever thread it
// spawns (matching the pattern established in Phase 3's tests), not a
// class that owns its own thread lifecycle; Phase 10's replay harness is
// expected to reuse this same class from its own driver loop.
class LiveConsumer {
public:
    LiveConsumer(tse::ingestion::SpscRingBuffer<tse::ingestion::IngestionEvent>& queue, LivePipeline& pipeline,
                 IAlertSink* alert_sink)
        : queue_(queue), pipeline_(pipeline), alert_sink_(alert_sink) {}

    // Pops and processes events until producer_done is observed AND the
    // queue is drained -- the same "one more drain attempt after the done
    // flag is seen" shape as Phase 3's two_thread_pipeline_test, so an
    // event pushed right before the producer signals completion is never
    // silently missed by a race between the flag and the last push.
    void run(const std::atomic<bool>& producer_done);

    const std::vector<EventLatencySample>& latency_samples() const { return latency_samples_; }
    uint64_t events_processed() const { return events_processed_; }
    uint64_t events_skipped_inconsistent() const { return events_skipped_inconsistent_; }

private:
    void process_one(const tse::ingestion::IngestionEvent& event);

    tse::ingestion::SpscRingBuffer<tse::ingestion::IngestionEvent>& queue_;
    LivePipeline& pipeline_;
    IAlertSink* alert_sink_;
    std::vector<EventLatencySample> latency_samples_;
    uint64_t events_processed_{0};
    uint64_t events_skipped_inconsistent_{0};
};

}  // namespace tse::pipeline
