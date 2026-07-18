# cpp/ml_client/ — async ML scoring: design decisions and measured proof

Companion to each class's own header comments. This is the narrative
version: why the module is split the way it is, the explicit failure-
handling contract, and the actual measured numbers from the real
graceful-degradation test the build guide's Phase 7 "Done when" asks for.

## Why three classes, not one

`MlScoreClient` (blocking HTTP + timeout), `MlScoringWorker` (the async
boundary: bounded queue + background thread), and `MlAnomalyDetector`
(the `IDetector`-conforming face) are three separate classes because they
have three separate jobs that must not leak into each other:

- `MlScoreClient` knows how to talk to `ml_service/` and nothing else. It
  blocks — deliberately — but that's only ever safe because of where it's
  called from.
- `MlScoringWorker` is what makes that blocking call safe: it owns the
  only thread that ever calls `MlScoreClient::score()`, decoupled from
  the hot path by a bounded, non-blocking queue.
- `MlAnomalyDetector` is the only one of the three that `LivePipeline`
  (Phase 6) ever touches directly, and it is designed so that nothing
  inside it *can* block — not "is careful not to," but structurally
  cannot, since `evaluate()` never does anything but update a small local
  map and call a non-blocking `submit()`.

This separation is what makes "async, off the hot path" (architecture doc
§3) a property of the code, checkable by reading `evaluate()`'s body,
rather than a promise about how the pieces happen to be wired together.

## The failure-handling contract, explicit (this is what Phase 7 asked for)

| Failure | What happens | Where |
|---|---|---|
| ML service takes longer than the configured timeout (200ms default) | `MlScoreClient::score()` returns `std::nullopt` at the timeout, never blocks past it | `ml_score_client.cpp` |
| ML service is down / connection refused | Same: `std::nullopt`, never throws | `ml_score_client.cpp` |
| ML service returns a non-200 status or an unparseable body | Same: `std::nullopt` — a "successful" HTTP response that isn't a valid score is treated identically to a failed one | `ml_score_client.cpp` |
| Any of the above | Counted via `requests_failed()`, no `Alert` produced for that window, worker moves on to the next queued request | `ml_scoring_worker.cpp` |
| The worker is behind (still processing, or the service is slow) and the hot path submits another request | The bounded queue (an `ingestion::SpscRingBuffer`) drops the *oldest* pending request — the same policy, same tested implementation, Phase 3 built for the FIX ingestion hot path | `ml_scoring_worker.hpp`, reusing `cpp/ingestion/spsc_ring_buffer.hpp` |
| Any of the above, from `LivePipeline`'s point of view | Nothing — `MlAnomalyDetector::evaluate()` never observes success or failure at all; it fires-and-forgets via `submit()` and always returns an empty vector | `ml_anomaly_detector.cpp` |

Net effect: **fail open**. A slow or dead ML service produces zero
additional alerts and — this is the part that's proven below, not just
argued — zero measurable impact on the hot path's own latency.

## Reusing Phase 3's ring buffer for a second, unrelated queue

`MlScoringWorker`'s request queue is a second instantiation of
`tse::ingestion::SpscRingBuffer<ScoringRequest>` — the exact class Phase 3
built (and Phase 6 found and fixed a real data race in — see
`cpp/ingestion/README.md`'s "Bug 3") for the FIX ingestion hot path. This
is deliberate reuse, not a coincidence of naming: the requirement here is
identical to Phase 3's original one — a single producer (the consumer
thread calling `MlAnomalyDetector::evaluate()`), a single consumer (the ML
worker thread), bounded capacity, and drop-oldest when the consumer falls
behind. Building a second, parallel implementation of the same policy
would only be a second place for the same class of bug to hide.

## Hand-rolled JSON, matching project convention

`ml_json.hpp`/`.cpp` implements exactly two shapes — the request
(`account_id`, `instrument_id`, `window_features`) and the response
(`anomaly_score`, `model_version`) — with direct string building for
encode and a small scoped parser for decode, rather than pulling in a
general-purpose JSON library. This matches the precedent already set by
`cpp/simulator/serialization/json_writer.cpp` (Phase 1) and
`cpp/ingestion/event_codec.cpp` (Phase 3): this codebase writes exactly
the JSON handling a fixed, known shape needs, not general infrastructure.

## Verification: unit tests, then the real thing

**Unit level** (`cpp/tests/ml_client/`, 26 tests): JSON encode/decode
round-trips and malformed-input rejection (`ml_json_test.cpp`);
`MlScoreClient` against a real in-process `httplib::Server` — genuine
success, genuine timeout (a handler that sleeps past the configured
timeout), genuine connection-refused (nothing listening), non-200 status,
and malformed body, each proven to return `std::nullopt` without throwing
(`ml_score_client_test.cpp`); `MlScoringWorker`'s alert-threshold logic,
failure counting, and the specific claim that `submit()` never blocks even
against a full queue with nothing draining it
(`ml_scoring_worker_test.cpp`); `MlAnomalyDetector`'s feature accumulation,
periodic (not per-order) submission, per-`(account, instrument)` window
isolation, and — the one that matters most — that `evaluate()` always
returns an empty vector regardless of what's happening on the other end of
the wire (`ml_anomaly_detector_test.cpp`).

**Integration level, the actual build-guide "Done when"**
(`graceful_degradation_test.cpp`): a real `ml_service/` subprocess (POSIX
fork/exec, not a mock — see `python_ml_service_process.hpp`), driven
through three real scenarios against the same 5,000-order stream, the same
`LivePipeline` (all 5 synchronous detectors *plus* `MlAnomalyDetector`),
and the same `LiveConsumer` latency instrumentation Phase 6 built:

**Benchmark:**

| Scenario | ml_scored | ml_failed | hot-path detectors_ns mean | hot-path detectors_ns p99 |
|---|---|---|---|---|
| Normal (fast, healthy) | 266 | 0 | 1737ns | 3291ns |
| Artificially slowed (1000ms delay vs. 150ms client timeout) | 0 | 257 | 1706ns | 3250ns |
| Killed mid-lifecycle (real process, hard `SIGKILL`'d after confirmed healthy) | 0 | 461 | 1761ns | 3542ns |

**ASan** (real numbers, fresh build, same three scenarios): mean 156016 /
156108 / 159631ns (normal / slow / killed) — again flat across all three,
comfortably under the 1ms budget once ASan's own instrumentation overhead
(~90x vs. benchmark, consistent with Phase 6's own detector-latency ASan
overhead) is accounted for.

**TSan** (real numbers, fresh build, same three scenarios, run with
`tsan_suppressions.txt` active): mean 413410 / 427041 / 411256ns (normal /
slow / killed) — flat again, comfortably under budget, and — the actual
point of running this under TSan at all — **zero data races reported** in
209/209 tests including this one, despite this test doing something new
relative to every earlier phase: `fork()`-ing a real child process from
inside a multi-threaded gtest binary and killing it mid-test
(`python_ml_service_process.cpp`).

Across all three sanitizer configs, the pattern is the same: absolute
latency scales with instrumentation overhead exactly as expected (and
matches Phase 6's own overhead ratios for the five-detector-only
baseline), but the *relative* flatness across normal/slow/killed — the
actual claim under test — holds in every config, not just the fast one.

The ML-side counters (`ml_scored`/`ml_failed`) differ exactly as expected
across the three scenarios — proving the slow/kill injection is genuinely
happening, not a no-op. The hot-path latency — the same `detectors_ns`
metric and the same budget (mean < 1ms, p99 < 5ms) Phase 6 established for
the five synchronous detectors alone — stays statistically indistinguishable
across all three (1737 / 1706 / 1761ns mean), comfortably inside budget in
every case. This is what "prove the hot path is unaffected" (build guide,
Phase 7) means made concrete: not "the code looks like it wouldn't block,"
but a measured number, from a real killed process, that didn't move.

Scenario 3 specifically kills a process that was already confirmed
healthy (not "never started") — the failure path exercised is "a running
dependency died," which is a meaningfully different and more realistic
condition than "was never reachable to begin with."
