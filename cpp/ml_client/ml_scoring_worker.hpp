#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "alert_sink.hpp"
#include "ml_score_client.hpp"
#include "scoring_request.hpp"
#include "spsc_ring_buffer.hpp"

namespace tse::ml_client {

struct MlScoringWorkerConfig {
    double alert_threshold{0.7};       // anomaly_score at/above this produces an Alert
    std::size_t queue_capacity{1024};  // must be a power of two >= 2 (SpscRingBuffer's own requirement)
};

// The actual async boundary: submit() is the only thing the hot path ever
// calls, and it never blocks — the bounded SpscRingBuffer underneath it
// is what makes that true structurally (drop-oldest under backpressure,
// exactly the policy Phase 3 built and proved race-free under sustained
// load — see cpp/ingestion/README.md), not just a documented convention.
// run() does the actual (potentially slow, up to the client's configured
// timeout) HTTP work, on whatever thread its caller spawns — the same
// "thread-agnostic plain method" shape as LiveConsumer::run()
// (cpp/pipeline/live_consumer.hpp), entirely decoupled from whichever
// thread calls submit().
//
// Failure handling, explicit, not left implicit (Phase 7's requirement):
//   - submit() when the queue is already full silently drops the oldest
//     pending request (visible via requests_dropped(), which forwards the
//     ring buffer's own dropped_count()) — the hot path is never made to
//     wait for the ML worker to catch up.
//   - A score() call that times out, can't connect, or returns a response
//     that doesn't parse counts against requests_failed() and produces no
//     Alert. The pipeline simply proceeds without that window's ML
//     opinion — fail open, never block, never crash — which is exactly
//     what "prove graceful degradation" (Phase 7) needs to be true of.
class MlScoringWorker {
public:
    MlScoringWorker(MlScoreClient client, tse::pipeline::IAlertSink* alert_sink, MlScoringWorkerConfig config = {});

    void submit(ScoringRequest request);

    // Drains until stop_flag is observed AND the queue is empty — the
    // same "one more drain attempt" shape as LiveConsumer::run(), so a
    // request submitted right before shutdown is signaled isn't silently
    // lost to a race between the flag and the last submit().
    void run(const std::atomic<bool>& stop_flag);

    uint64_t requests_scored() const { return scored_.load(std::memory_order_relaxed); }
    uint64_t requests_alerted() const { return alerted_.load(std::memory_order_relaxed); }
    uint64_t requests_failed() const { return failed_.load(std::memory_order_relaxed); }
    uint64_t requests_dropped() const { return queue_.dropped_count(); }

private:
    void process_one(const ScoringRequest& request);

    MlScoreClient client_;
    tse::pipeline::IAlertSink* alert_sink_;
    MlScoringWorkerConfig config_;
    tse::ingestion::SpscRingBuffer<ScoringRequest> queue_;
    std::atomic<uint64_t> scored_{0};
    std::atomic<uint64_t> alerted_{0};
    std::atomic<uint64_t> failed_{0};
};

}  // namespace tse::ml_client
