# FIX-Native Trade Surveillance Engine — Architecture Spec

Companion to `P2_FIX_Native_Trade_Surveillance_Engine_HLD.md` (decisions/rationale). This doc is the concrete technical reference: repo layout, module responsibilities, interfaces, and data flow.

---

## 1. Repo layout

```
trade-surveillance-engine/
├── P2_FIX_Native_Trade_Surveillance_Engine_HLD.md
├── ARCHITECTURE.md                 (this file)
├── CMakeLists.txt
├── docker-compose.yml              (TimescaleDB + Kafka)
├── cpp/
│   ├── fix/                        # Phase 2 — QuickFIX session + parsing
│   ├── ingestion/                  # Phase 3 — SPSC ring buffer + Kafka producer/consumer
│   ├── orderbook/                  # Phase 4 — price-time priority book engine
│   ├── detectors/                  # Phase 5 — WashTrade, SpoofingLayering, MarkingTheClose,
│   │                                #           FrontRunning, StatisticalBaseline
│   ├── pipeline/                   # Phase 6 — the single wiring of ingestion/orderbook/detectors
│   │                                #           together, used by both live mode and (Phase 10)
│   │                                #           harness/'s replay mode — no separate offline
│   │                                #           scoring implementation exists or is planned
│   ├── ml_client/                  # Phase 7 — async HTTP client for ml_service/: bounded queue +
│   │                                #           worker thread + IDetector shim, all off the hot path
│   ├── simulator/                  # Phase 1 — synthetic order/execution generator
│   ├── db/                         # Phase 8 — libpqxx / TimescaleDB integration: schema
│   │                                #           (orders/trades/alerts hypertables), AlertStore
│   │                                #           (insert + the query surface api/ calls),
│   │                                #           DbAlertSink (the real IAlertSink pipeline/'s
│   │                                #           own comments have referred to since Phase 6)
│   ├── api/                        # Phase 9 — Crow REST layer over db/: alert listing/
│   │                                #           filtering/case-status, LiveBookRegistry (the
│   │                                #           thread-safety layer that lets HTTP handler
│   │                                #           threads read book state while LivePipeline is
│   │                                #           still running on its own thread), and
│   │                                #           main.cpp — the live demo server entrypoint
│   ├── harness/                    # Phase 10-11 — evaluation harness, replay, metrics
│   └── tests/                      # GoogleTest, mirrors the above module structure
├── ml_service/                     # Phase 7 — Python FastAPI + Isolation Forest (fully independent)
│   ├── app/
│   ├── tests/                      # pytest
│   └── requirements.txt
├── dashboard/                      # Phase 9 — React + TypeScript (Vite, own npm project):
│   │                                #           live ticker, alert queue, severity-coded
│   │                                #           alert cards, order book depth, event
│   │                                #           timeline, compliance action buttons
│   └── src/
└── calibration/                    # Phase 11 — offline WRDS/TAQ calibration scripts (NOT shipped data)
    └── README.md                   # notes only — no raw/derived TAQ data ever committed here
```

---

## 2. Module responsibilities

| Module | Responsibility | Depends on |
|---|---|---|
| `simulator/` | Generates synthetic Order/Execution events, multi-asset, parameterized severity for injected abuse patterns, emits ground-truth labels (strippable) | Nothing (pure generator) |
| `fix/` | QuickFIX session management (sequence numbers, heartbeats), parses `NewOrderSingle`/`ExecutionReport`/`OrderCancelRequest` into internal Order/Execution structs | `simulator/` (as a message source in dev/test) |
| `ingestion/` | SPSC ring buffer (in-process hot path) + Kafka producer/consumer (durability, deterministic replay) | `fix/` |
| `orderbook/` | Hand-rolled book, price-time priority, per-instrument state, depth snapshotting | `ingestion/` |
| `detectors/` | One class per detector, common `IDetector` interface, run inline against live book state | `orderbook/` |
| `pipeline/` | **The single reusable code path both live mode and Phase 10's replay mode run through — there is no separate offline scoring script.** `LivePipeline` does per-instrument `OrderBook` routing, runs all registered detectors against each event, and measures book-apply/detector latency separately; `LiveConsumer` owns the ring-buffer pop loop (thread-agnostic — a plain method any caller's thread invokes, not a self-threading class), which is exactly what lets `harness/` drive the identical class from its own replay driver instead of reimplementing detector orchestration; `PipelineEventSink` bridges `fix/`'s `IEventSink` hook into `ingestion/`'s ring buffer + Kafka | `ingestion/`, `orderbook/`, `detectors/`, `fix/` |
| `ml_client/` | `MlAnomalyDetector` — an `IDetector` that only ever accumulates volume/frequency window features and issues a non-blocking `submit()`, never returns anything but an empty vector; `MlScoringWorker` — owns the bounded request queue (reuses `ingestion/`'s `SpscRingBuffer`, same drop-oldest policy) and a background thread that does the actual (blocking-with-timeout) HTTP call and forwards any resulting Alert straight to `IAlertSink`; `MlScoreClient` — the blocking HTTP call itself, timeout-bounded, never called except from that worker thread | `ingestion/`, `orderbook/`, `detectors/`, `fix/`, `pipeline/` (for `IAlertSink` only — `pipeline/` has no dependency back on `ml_client/`) |
| `db/` | `AlertStore` — the only class in this project that speaks SQL: `apply_schema()` (idempotent DDL), `insert_order`/`insert_execution`/`insert_alert`, the query surface the build guide's Phase 8 "Done when" names explicitly (time-range, filter-by-account, filter-by-detector-type) plus Phase 9's `list_recent_alerts`/`get_alert`/`update_alert_status` (case-management state — `alerts.status`, `OPEN`→`UNDER_REVIEW`/`ESCALATED`→`CLOSED`, enforced by a schema CHECK constraint, never by a detector). `DbAlertSink` — the real `IAlertSink` implementation, writing synchronously (no live production entrypoint puts this on the hot consumer thread yet; if one does, `ml_client/`'s async bounded-queue-plus-worker-thread pattern is the template to reuse, not built speculatively here) | `pipeline/` |
| `api/` | `AlertStore`-backed REST endpoints (listing/filtering/case-status) plus `LiveBookRegistry` — a thread-safe wrapper `LivePipeline` itself deliberately isn't (see `pipeline/`'s own entry): the same ring-buffer pop-and-process shape as `LiveConsumer`, mutex-shared between the one thread that calls `process()` and any number of HTTP handler threads calling `snapshot()`. `main.cpp` is the live demo server — a real FIX loopback session feeding a real `LivePipeline` + `DbAlertSink`, the actual "Done when" entrypoint Phase 9's dashboard runs against | `db/` |
| `harness/` | `ground_truth.cpp` indexes Phase 1 labels by order/trade id before stripping (evaluation-only, never on the replay path); `replay_runner.cpp` publishes simulator output as `fix::Order`/`Execution` to a real Kafka topic and replays it deterministically into an unmodified `LiveConsumer`/`LivePipeline`; `evaluation.cpp` scores the resulting Alerts against ground truth per detector's own evidence-id universe (confusion matrix, threshold sweep, severity gradient). `tse_harness_eval` is the "Done when" entrypoint — see `cpp/harness/README.md` for real numbers and honest findings, including two real bugs this phase found and fixed (`MarkingTheCloseDetector` never populated `Alert::order_ids`; `FrontRunningDetector`'s leader/large-order ordering requirement was backwards relative to both the generator and the standard front-running definition) and one Phase 1/Phase 5 calibration mismatch left for Phase 11 rather than patched here (`SpoofingLayeringDetector`'s speed signal vs. the generator's dwell-time formula) | `simulator/`, `ingestion/`, `pipeline/`, `detectors/` |
| `ml_service/` | FastAPI + scikit-learn Isolation Forest, trained at process startup on synthetic volume/frequency baseline features (never real data — see CLAUDE.md); scores `POST /score` requests from `ml_client/` | Independent — own data contract only, no shared build tooling with the rest of the repo |
| `dashboard/` | React + TypeScript UI (own Vite project), reads from `api/` only via polling (no direct backend access, no WebSocket push — matches §4's "pull, not push, for v1") | `api/` |
| `calibration/` | Offline scripts pulling WRDS/TAQ stylized facts, outputs parameter values consumed by `simulator/` config — never commits raw data | External (WRDS, not shipped) |

---

## 3. Key interfaces

### `IDetector` (common interface for all detectors, `detectors/`)
```cpp
using DetectorEvent = std::variant<Order, Execution>;

class IDetector {
public:
    virtual ~IDetector() = default;
    virtual std::vector<Alert> evaluate(const OrderBook& book,
                                          const DetectorEvent& incoming,
                                          const AccountRegistry& accounts) = 0;
    virtual std::string name() const = 0;
};
```
Every detector — including `StatisticalBaselineDetector` — implements this. This is what lets the evaluation harness (Phase 10) swap/compare detectors uniformly and produce an apples-to-apples precision/recall comparison.

`incoming` takes `Order` *or* `Execution` (revised from an earlier `Order`-only draft during Phase 5 implementation): `WashTradeDetector` and `MarkingTheCloseDetector` are both fundamentally about completed trades, not resting order state, and can't be implemented correctly against Order-only input. Detectors that only need Order data simply never match the Execution arm.

### `IngestionQueue` (`ingestion/`)
SPSC ring buffer with a fixed-capacity backing array, plus a Kafka-backed durable layer behind it. Backpressure policy (drop-oldest, block, or grow — pick one in Phase 3 and document the choice) lives here, not scattered across callers.

### `OrderBook` (`orderbook/`)
Exposes `apply(const Order&)`, `apply(const Execution&)`, `snapshot() -> DepthSnapshot`, and read-only depth-at-level queries. Detectors depend only on this interface, never on ingestion internals — keeps detection logic testable in isolation with hand-constructed book states.

### `IEventSink` (`fix/`) and `LivePipeline`/`LiveConsumer` (`pipeline/`)
```cpp
class IEventSink {
public:
    virtual ~IEventSink() = default;
    virtual void on_order(const Order& order) = 0;
    virtual void on_execution(const Execution& execution) = 0;
};
```
`fix/` owns this abstraction (added in Phase 6) rather than depending on `ingestion/` directly — `SurveillanceFixApplication::set_event_sink()` calls it for every parsed message. `pipeline/`'s `PipelineEventSink` is the concrete implementation, bridging the callback into the SPSC ring buffer (and, optionally, Kafka). `LivePipeline::process(event)` then routes each event to its per-instrument `OrderBook`, runs every registered `IDetector` against the result, and measures book-apply/detector latency separately. `LiveConsumer` owns the ring-buffer pop loop that drives it — thread-agnostic (a plain method a caller runs on whatever thread it spawns), so Phase 10's replay harness can reuse the exact same class from its own driver loop, which is what makes "replay mode reuses the exact same detection code path as live mode" (§5) literally true rather than just a design intention.

### `LiveBookRegistry` (`api/`)
```cpp
class LiveBookRegistry {
public:
    LiveBookRegistry(SpscRingBuffer<IngestionEvent>& queue, LivePipeline& pipeline,
                      IAlertSink* alert_sink);
    void run(const std::atomic<bool>& producer_done);
    std::optional<DepthSnapshot> snapshot(const std::string& instrument_id);
    uint64_t events_processed() const;
private:
    std::mutex pipeline_mutex_;
    std::atomic<uint64_t> events_processed_{0};
};
```
`LivePipeline` is deliberately, explicitly not thread-safe (its own header
comment: "exactly one consumer thread ever calls `process()`") — true
through Phase 8, where every caller was a test or a `LiveConsumer` joined
before anything else touched the result. Phase 9's dashboard is the first
caller that needs to *read* book state (`GET /api/orderbook/.../snapshot`)
from HTTP handler threads while a live pipeline is still running
concurrently on its own thread. `LiveBookRegistry` is the same
ring-buffer pop-and-process shape as `LiveConsumer` — deliberately a
small, separate class rather than a modification to `LiveConsumer` itself,
since three earlier phases' tests already depend on that class's exact
current (non-thread-safe) shape — but every `process()` call and every
`snapshot()` read share one mutex, which is what makes the cross-thread
read race-free without changing `LivePipeline`'s own contract at all.

`events_processed()`/`events_skipped_inconsistent()` are `std::atomic`,
not plain `uint64_t` — found necessary the hard way: a real TSan run
during this phase's own test development caught a genuine data race
between `process_one()`'s `++events_processed_` and a test polling
`events_processed()` from a different thread while `run()` was still
active, a cross-thread-concurrent-read pattern `LiveConsumer`'s own tests
never exercised (see `cpp/api/README.md`).

### ML microservice contract (`ml_service/`)
```
POST /score
{ "account_id": ..., "instrument_id": ..., "window_features": {...} }
→ { "anomaly_score": float, "model_version": string }
```
Called async — never on the synchronous hot path. The path from `detectors/`
to this contract runs entirely through `ml_client/`, not `detectors/`
directly: `MlAnomalyDetector` (an `IDetector`) only ever accumulates
features and calls a non-blocking `submit()`; `MlScoringWorker`'s own
background thread is what actually issues this HTTP call, via
`MlScoreClient`.

**Timeout + fallback behavior, defined explicitly (Phase 7), not left
implicit:**
- `MlScoreClient` uses a strict connect + read timeout (200ms default).
  Any failure — timeout, connection refused, non-200 status, or an
  unparseable response — returns `std::nullopt`, never throws: "the ML
  service is unavailable" is an ordinary, expected outcome, not an
  exceptional one.
- `MlScoringWorker.submit()` is non-blocking by construction: it pushes
  onto a bounded `ingestion::SpscRingBuffer`, which reuses the exact same
  drop-oldest policy Phase 3 built and TSan-verified — a queue-full
  condition (the worker falling behind, or the service being down) drops
  the oldest pending request rather than making the hot path wait.
- Net effect: fail open. A slow or dead ML service produces zero
  additional alerts and zero hot-path latency impact — never a block,
  never a crash. Proven, not just asserted, by
  `cpp/tests/ml_client/graceful_degradation_test.cpp`: a real `ml_service/`
  subprocess, artificially slowed past its client timeout and then hard-
  killed, with the hot-path latency budget (the same one Phase 6
  established for the five synchronous detectors) measured to hold in
  both cases.

### Alert evidence contract (`db/`)
Every `Alert` written to TimescaleDB carries enough evidence to reconstruct *why* it fired without re-running the pipeline: detector name, order/trade IDs involved, a book depth snapshot reference, and the time window. Concretely, as of Phase 8: `detectors::Alert` (`cpp/detectors/alert.hpp`) carries `model_version` (populated only by `MlAnomalyDetector`-sourced alerts, `std::nullopt` for every deterministic-rule detector) and `book_snapshot_sequence` (`OrderBook::sequence()` at the moment of firing — every current detector populates it, including the async ML path, which threads it through `ScoringRequest` since `MlScoringWorker`'s background thread has no `OrderBook` reference of its own) as first-class `std::optional` fields, not values embedded only in the free-text `evidence` string. `alerts`' schema stores `account_ids`/`order_ids` as native Postgres arrays (an alert can implicate more than one account or order — e.g. `WashTradeDetector`'s two related accounts) and indexes `account_ids` with a GIN index for the account-filter query. `alerts.status` (Phase 9) is case-management state layered on top at the persistence level only — `OPEN` by default, `UNDER_REVIEW`/`ESCALATED`/`CLOSED` reachable via `api/`'s `PATCH /api/alerts/:id/status`, enforced by a schema `CHECK` constraint rather than a detector-side enum, matching `detectors::Alert`'s own header comment that this field is deliberately not something a detector could ever populate.

### REST API contract (`api/`)
```
GET   /api/health
GET   /api/alerts[?account_id=|?detector_name=|?start_ns=&end_ns=][&limit=]
GET   /api/alerts/<id>
PATCH /api/alerts/<id>/status   { "status": "..." }
GET   /api/orderbook/<instrument_id>/snapshot
```
Every alert-listing/filter query maps directly onto one of `db/`'s Phase 8
query methods (mutually exclusive, checked in the order listed — one
filter per request, not a combinator). The order-book endpoint is the one
route not backed by `db/` at all: it reads live, in-memory `OrderBook`
state through `api/`'s `LiveBookRegistry`, the thread-safety layer that
lets HTTP handler threads read book state safely while a real
`LivePipeline` is still running concurrently on its own thread (see
`api/`'s module entry in §2) — 503 if no live pipeline is attached to the
process, 404 if the instrument has never traded. CORS is enabled
(permissive — this is a local, unauthenticated dev API, same posture as
Kafka/TimescaleDB) so `dashboard/`, served from a different origin by
Vite's dev server, can call it directly from the browser.

---

## 4. Data flow (single order lifecycle, live mode)

```
FIX message arrives
  → fix/ parses into Order/Execution struct, hands it to IEventSink
  → pipeline/'s PipelineEventSink pushes it onto ingestion/'s SPSC ring
    buffer (hot path) and, if configured, publishes it to Kafka
    (durability, async) — both fed at arrival, independently
  → pipeline/'s LiveConsumer (its own thread) pops the event and calls
    LivePipeline::process()
  → orderbook/ applies the update to that instrument's OrderBook, produces
    new book state (LivePipeline routes by instrument_id)
  → detectors/ each IDetector.evaluate() runs against the updated book
       (StatisticalBaselineDetector and the other four run fully
        synchronously; ml_client/'s MlAnomalyDetector only ever
        accumulates features and calls MlScoringWorker::submit() —
        non-blocking — before returning)
  → any Alert objects produced by the five synchronous detectors flow to
    an IAlertSink immediately; MlAnomalyDetector's own Alert (if any)
    arrives later, from MlScoringWorker's separate background thread,
    directly to the same IAlertSink — db/'s DbAlertSink (Phase 8) is the
    real persistence implementation, writing synchronously to TimescaleDB;
    Phase 6 also has CollectingAlertSink for tests/demonstration
  → api/'s live demo server (main.cpp, Phase 9) is the first entrypoint
    that actually runs this whole path continuously: a real FIX loopback
    session feeds pipeline/'s ring buffer, api/'s LiveBookRegistry (not
    LiveConsumer directly — see its own module entry above) drains it into
    a real LivePipeline + DbAlertSink, and api/'s Crow server exposes the
    result at /api/alerts and /api/orderbook/.../snapshot
  → api/ serves alerts and book snapshots to dashboard/ on request (pull,
    not push, for v1 — dashboard/ polls every 1.5-3s per panel)
```
A ring-buffer drop can make a *logically* inconsistent event reach `OrderBook` (e.g. an Execution referencing an order whose New was dropped) — `OrderBook::apply()` throws `std::invalid_argument` for exactly this (Phase 4's documented stance: a genuine invariant violation in a hand-constructed single-threaded sequence). `LivePipeline::process()` catches that specific exception, counts it, and skips detector evaluation for that event rather than crashing the consumer thread — an accepted, already-documented consequence of Phase 3's drop-oldest policy, not a defect.

## 5. Data flow (evaluation/replay mode — Phase 10-11)

```
simulator/ generates a labeled SimulationOutput (severity parameter set)
  → harness/'s ground_truth.cpp indexes every Order/Execution's
    GroundTruthLabel by order_id/trade_id BEFORE stripping -- evaluation-
    only bookkeeping, never touching the replay path below
  → harness/'s replay_runner.cpp converts to plain fix::Order/Execution
    (Phase 2's live-mode structs -- no ground_truth_label field exists on
    them at all) and publishes to a real Kafka topic via ingestion/'s
    KafkaProducer
  → ingestion/'s KafkaReplayConsumer seeks to the beginning and reads it
    back deterministically into a fresh SpscRingBuffer
  → pipeline/'s LiveConsumer (unmodified -- the same class the live path
    uses) pops that buffer and drives pipeline/'s LivePipeline (also
    unmodified), wired with the five Phase 5 rule-based detectors
    (MlAnomalyDetector excluded -- an out-of-band async HTTP call would
    reintroduce the nondeterminism KafkaReplayConsumer exists to avoid)
  → harness/'s evaluation.cpp scores the resulting Alerts against the
    ground-truth index: each detector's confusion matrix is computed over
    its own "evidence universe" (the exact id space it can reference --
    e.g. SpoofingLayeringDetector only ever fires from handle_cancel(), so
    its universe is Cancel-order ids, not all orders), not a single
    generic id space shared across detectors
  → precision/recall/F1, threshold sweep, confusion matrix, and a
    severity-gradient curve are printed by tse_harness_eval -- see
    cpp/harness/README.md for the real numbers and, importantly, the
    honest findings: two real bugs this phase found and fixed
    (MarkingTheCloseDetector never populated Alert::order_ids; and
    FrontRunningDetector required its smaller "leader" order to precede
    the large order it front-runs, which structurally could never match
    what abuse/front_running.cpp actually generates -- the generator's
    leader-trades-shortly-AFTER-the-large-order-is-placed shape is the one
    that matches the standard regulatory front-running definition
    (advance knowledge of a *pending* order, not of a not-yet-placed one);
    fixing the detector's direction took its recall on this run from
    0.000 to 0.578), plus one Phase 1/Phase 5 calibration mismatch left
    for Phase 11 rather than patched here (SpoofingLayeringDetector's
    speed_score signal is handicapped below severity ≈0.92 given the
    generator's current dwell-time formula -- exactly the kind of gap
    Phase 11's real-data recalibration exists to close).
```
Note what does *not* appear above: fix/'s QuickFIX session/wire layer.
Replay mode reuses the exact same **detection** code path as live mode
(ingestion/'s ring buffer and Kafka, pipeline/'s LiveConsumer/LivePipeline,
detectors/ unmodified) — but skips FIX wire encode/decode entirely, going
straight from simulator output to the same `fix::Order`/`Execution` structs
FIX parsing would have produced. Phase 2's FIX layer already has its own
dedicated round-trip tests (`cpp/tests/fix/`); re-deriving detection
correctness through a wire-protocol hop this phase doesn't need would only
add nondeterminism risk (network loopback timing) without adding coverage
of anything this phase is scoped to measure.

---

## 6. Conventions

- C++17 throughout `cpp/`, no exceptions across the `IDetector` boundary (return empty vector on no-alert, don't throw for "nothing found")
- All timestamps UTC, nanosecond precision where the source data supports it (FIX/execution reports), stored as `int64_t` epoch nanos
- No raw or derived WRDS/TAQ data committed anywhere in the repo, including `calibration/`
- `ground_truth_label` fields must never appear in any code path that would run against real (non-synthetic) data
