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
│   ├── simulator/                  # Phase 1 — synthetic order/execution generator
│   ├── db/                         # Phase 8 — libpqxx / TimescaleDB integration
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
| `db/` | Alert/order/trade persistence, TimescaleDB hypertables, query layer | `detectors/` |
| `api/` | REST endpoints over `db/` — alert listing/filtering, book snapshot retrieval | `db/` |
| `harness/` | Replays labeled synthetic scenarios through the real pipeline via Kafka, computes precision/recall/F1, threshold sweep, difficulty curve | `simulator/`, `ingestion/`, `detectors/` |
| `ml_service/` | Isolation Forest anomaly scoring, called async/out-of-band from `detectors/` over REST/gRPC | Independent — own data contract only |
| `dashboard/` | React UI, reads from `api/` only, no direct backend access | `api/` |
| `calibration/` | Offline scripts pulling WRDS/TAQ stylized facts, outputs parameter values consumed by `simulator/` config — never commits raw data | External (WRDS, not shipped) |

---

## 3. Key interfaces

### `IDetector` (common interface for all detectors, `detectors/`)
```cpp
class IDetector {
public:
    virtual ~IDetector() = default;
    virtual std::vector<Alert> evaluate(const OrderBook& book,
                                          const Order& incoming,
                                          const AccountRegistry& accounts) = 0;
    virtual std::string name() const = 0;
};
```
Every detector — including `StatisticalBaselineDetector` — implements this. This is what lets the evaluation harness (Phase 10) swap/compare detectors uniformly and produce an apples-to-apples precision/recall comparison.

### `IngestionQueue` (`ingestion/`)
SPSC ring buffer with a fixed-capacity backing array, plus a Kafka-backed durable layer behind it. Backpressure policy (drop-oldest, block, or grow — pick one in Phase 3 and document the choice) lives here, not scattered across callers.

### `OrderBook` (`orderbook/`)
Exposes `apply(const Order&)`, `apply(const Execution&)`, `snapshot() -> DepthSnapshot`, and read-only depth-at-level queries. Detectors depend only on this interface, never on ingestion internals — keeps detection logic testable in isolation with hand-constructed book states.

### ML microservice contract (`ml_service/`)
```
POST /score
{ "account_id": ..., "instrument_id": ..., "window_features": {...} }
→ { "anomaly_score": float, "model_version": string }
```
Called async from `detectors/` — never on the synchronous hot path. Timeout + fallback behavior must be defined in Phase 7, not left implicit.

### Alert evidence contract (`db/`)
Every `Alert` written to TimescaleDB carries enough evidence to reconstruct *why* it fired without re-running the pipeline: detector name, order/trade IDs involved, a book depth snapshot reference, and the time window.

---

## 4. Data flow (single order lifecycle, live mode)

```
FIX message arrives
  → fix/ parses into Order/Execution struct
  → ingestion/ SPSC ring buffer (hot path) + Kafka (durability, async)
  → orderbook/ applies the update, produces new book state
  → detectors/ each IDetector.evaluate() runs against the updated book
       (StatisticalBaselineDetector runs synchronously;
        ml_service/ call happens async, off this path)
  → any Alert objects produced flow to db/ for persistence
  → api/ serves alerts to dashboard/ on request (pull, not push, for v1)
```

## 5. Data flow (evaluation/replay mode — Phase 10-11)

```
simulator/ generates labeled scenario (severity parameter set)
  → written to Kafka topic (same topic type as live mode, different partition/tag)
  → harness/ replays it through the identical fix/ → ingestion/ → orderbook/ → detectors/ path
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
