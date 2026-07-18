#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "alert_sink.hpp"
#include "depth_snapshot.hpp"
#include "ingestion_event.hpp"
#include "live_pipeline.hpp"
#include "spsc_ring_buffer.hpp"

namespace tse::api {

// The api/-specific consumer loop: the same ring-buffer pop-and-process
// shape as pipeline/'s own LiveConsumer (Phase 6), but with every
// LivePipeline::process() call wrapped in a mutex -- the one piece of
// synchronization LivePipeline deliberately doesn't provide itself (see
// live_pipeline.hpp: "not thread-safe, by design... exactly one consumer
// thread ever calls process()"). That single-writer invariant is still
// true here -- only this class's run() ever calls process() -- but Phase 9
// adds a second kind of caller that needs to *read* the resulting book
// state from a different thread entirely: HTTP handler threads, via
// snapshot() below. The mutex is what makes that read race-free against
// the concurrent writer, without changing LivePipeline's own contract at
// all.
//
// Deliberately not built by modifying or templating LiveConsumer itself:
// LiveConsumer's implementation is a handful of lines, and three earlier
// phases' tests already depend on its exact current (non-thread-safe,
// unmodified LivePipeline&) shape. Duplicating that small shape here, with
// the mutex genuinely new logic added, is lower-risk than changing a class
// this project has already built three phases of proof on top of.
class LiveBookRegistry {
public:
    LiveBookRegistry(tse::ingestion::SpscRingBuffer<tse::ingestion::IngestionEvent>& queue,
                      tse::pipeline::LivePipeline& pipeline, tse::pipeline::IAlertSink* alert_sink)
        : queue_(queue), pipeline_(pipeline), alert_sink_(alert_sink) {}

    // Same "one more drain attempt after producer_done is observed" shape
    // as LiveConsumer::run() -- see that class for why. For the live demo
    // server (cpp/api/main.cpp), producer_done never becomes true during
    // normal operation; the process simply runs until stopped.
    void run(const std::atomic<bool>& producer_done);

    // Thread-safe: takes the same mutex run() holds around each
    // process() call. Returns nullopt if this instrument has never had an
    // event applied (mirrors LivePipeline::book_for()'s own nullptr case).
    std::optional<tse::orderbook::DepthSnapshot> snapshot(const std::string& instrument_id);

    // atomic, not plain uint64_t: unlike LiveConsumer's own counters (read
    // only after joining the consumer thread in every existing caller),
    // this class's whole reason to exist is being queried *while* run() is
    // still active on another thread -- a real TSan-caught data race here
    // during test development (a poll loop reading events_processed()
    // concurrently with process_one()'s ++events_processed_) is exactly
    // what these two fields being plain, unsynchronized integers produced.
    uint64_t events_processed() const { return events_processed_.load(std::memory_order_relaxed); }
    uint64_t events_skipped_inconsistent() const {
        return events_skipped_inconsistent_.load(std::memory_order_relaxed);
    }

private:
    void process_one(const tse::ingestion::IngestionEvent& event);

    tse::ingestion::SpscRingBuffer<tse::ingestion::IngestionEvent>& queue_;
    tse::pipeline::LivePipeline& pipeline_;
    tse::pipeline::IAlertSink* alert_sink_;
    std::mutex pipeline_mutex_;
    std::atomic<uint64_t> events_processed_{0};
    std::atomic<uint64_t> events_skipped_inconsistent_{0};
};

}  // namespace tse::api
