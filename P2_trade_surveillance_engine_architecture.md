# FIX-Native Trade Surveillance Engine — Architecture Spec

Companion to `P2_FIX_Native_Trade_Surveillance_Engine_HLD.md` (decisions/rationale) and `P2_trade_surveillance_engine_build_guide.md` (phased build steps). This doc is the concrete technical reference: repo layout, module responsibilities, interfaces, and data flow.

---

## 1. Repo layout

```
trade-surveillance-engine/
├── CLAUDE.md
├── P2_FIX_Native_Trade_Surveillance_Engine_HLD.md
├── P2_trade_surveillance_engine_build_guide.md
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
│   ├── api/                        # Phase 9 — Drogon or Crow REST layer
│   ├── harness/                    # Phase 10-11 — evaluation harness, replay, metrics
│   └── tests/                      # GoogleTest, mirrors the above module structure
├── ml_service/                     # Phase 7 — Python FastAPI + Isolation Forest (fully independent)
│   ├── app/
│   ├── tests/                      # pytest
│   └── requirements.txt
├── dashboard/                      # Phase 9 — React app (own npm project)
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
| `db/` | `AlertStore` — the only class in this project that speaks SQL: `apply_schema()` (idempotent DDL), `insert_order`/`insert_execution`/`insert_alert`, and the query surface the build guide's Phase 8 "Done when" names explicitly (time-range, filter-by-account, filter-by-detector-type). `DbAlertSink` — the real `IAlertSink` implementation, writing synchronously (no live production entrypoint puts this on the hot consumer thread yet; if one does, `ml_client/`'s async bounded-queue-plus-worker-thread pattern is the template to reuse, not built speculatively here) | `pipeline/` |
| `api/` | REST endpoints over `db/` — alert listing/filtering, book snapshot retrieval | `db/` |
| `harness/` | Replays labeled synthetic scenarios through the real pipeline via Kafka, computes precision/recall/F1, threshold sweep, difficulty curve | `simulator/`, `ingestion/`, `pipeline/` |
| `ml_service/` | FastAPI + scikit-learn Isolation Forest, trained at process startup on synthetic volume/frequency baseline features (never real data — see CLAUDE.md); scores `POST /score` requests from `ml_client/` | Independent — own data contract only, no shared build tooling with the rest of the repo |
| `dashboard/` | React UI, reads from `api/` only, no direct backend access | `api/` |
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
Every `Alert` written to TimescaleDB carries enough evidence to reconstruct *why* it fired without re-running the pipeline: detector name, order/trade IDs involved, a book depth snapshot reference, and the time window. Concretely, as of Phase 8: `detectors::Alert` (`cpp/detectors/alert.hpp`) carries `model_version` (populated only by `MlAnomalyDetector`-sourced alerts, `std::nullopt` for every deterministic-rule detector) and `book_snapshot_sequence` (`OrderBook::sequence()` at the moment of firing — every current detector populates it, including the async ML path, which threads it through `ScoringRequest` since `MlScoringWorker`'s background thread has no `OrderBook` reference of its own) as first-class `std::optional` fields, not values embedded only in the free-text `evidence` string. `alerts`' schema stores `account_ids`/`order_ids` as native Postgres arrays (an alert can implicate more than one account or order — e.g. `WashTradeDetector`'s two related accounts) and indexes `account_ids` with a GIN index for the account-filter query.

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
    Phase 6 also has CollectingAlertSink for tests/demonstration. No live
    production entrypoint wires DbAlertSink onto LiveConsumer's hot thread
    yet — that's a Phase 9+ decision, not made here (see db/README.md)
  → api/ serves alerts to dashboard/ on request (pull, not push, for v1)
```
A ring-buffer drop can make a *logically* inconsistent event reach `OrderBook` (e.g. an Execution referencing an order whose New was dropped) — `OrderBook::apply()` throws `std::invalid_argument` for exactly this (Phase 4's documented stance: a genuine invariant violation in a hand-constructed single-threaded sequence). `LivePipeline::process()` catches that specific exception, counts it, and skips detector evaluation for that event rather than crashing the consumer thread — an accepted, already-documented consequence of Phase 3's drop-oldest policy, not a defect.

## 5. Data flow (evaluation/replay mode — Phase 10-11)

```
simulator/ generates labeled scenario (severity parameter set)
  → written to Kafka topic (same topic type as live mode, different partition/tag)
  → harness/ replays it through the identical fix/ → ingestion/ → pipeline/ → detectors/ path
    (harness/ reuses pipeline/'s LiveConsumer/LivePipeline directly, not a separate replay-mode copy)
  → harness/ compares detector output Alerts against ground-truth labels
  → precision/recall/F1, threshold sweep, confusion matrix, difficulty-curve output
```
The critical property: replay mode reuses the *exact same* detection code path as live mode.

---

## 6. Conventions

- C++17 throughout `cpp/`, no exceptions across the `IDetector` boundary (return empty vector on no-alert, don't throw for "nothing found")
- All timestamps UTC, nanosecond precision where the source data supports it (FIX/execution reports), stored as `int64_t` epoch nanos
- No raw or derived WRDS/TAQ data committed anywhere in the repo, including `calibration/`
- `ground_truth_label` fields must never appear in any code path that would run against real (non-synthetic) data
