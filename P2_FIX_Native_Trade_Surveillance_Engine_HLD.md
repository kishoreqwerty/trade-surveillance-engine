# FIX-Native Trade Surveillance Engine (P2) — High-Level Design

**Language:** C++17 (locked, core engine) + Python (isolated ML microservice) + React (dashboard)
**Companion project:** Low-Latency Market Data Feed Handler (C++20/23, pre-trade, lock-free SPSC, sharded by instrument).

---

## 0. Relationship to the Feed Handler — deliberate overlap, explicit differentiation

This design accepts real overlap with the Feed Handler at the infrastructure layer: both use lock-free/low-latency ingestion and both maintain live state. That overlap was a conscious trade-off (chosen over a narrower batch-only design) in exchange for a bigger, more production-realistic system. To keep the two projects distinct in an interview narrative rather than reading as the same project twice, hold onto these differences and lead with them when asked "how is this different from your other C++ project":

| | Feed Handler | P2 (this project) |
|---|---|---|
| Direction | Pre-trade — exchange market data in | Post-trade — own OMS execution/order flow in, via FIX |
| Protocol | Raw exchange market data feed (binary/proprietary framing) | FIX 4.2 (industry-standard order/execution protocol) |
| Core hard problem | Ingestion throughput + lock-free correctness under contention | Live detection-logic correctness under a false-positive/negative tradeoff, running stateful rules against continuously-updating book state |
| What "done" means | Sanitizer-clean under load, verified latency percentiles | Precision/recall/F1 against labeled ground truth, verified detection logic |
| Downstream consumer | A trading decision engine (not built — out of scope) | A compliance analyst (alert queue, case management) |

The story: Feed Handler proves you can build low-latency infrastructure. P2 proves you can build low-latency infrastructure **and** put a correctness-critical, regulator-facing decision system on top of it. That combination is the differentiator, not a liability — but only if you can articulate it, so keep this table close.

---

## 1. Problem Statement

Ingest live order/execution flow via FIX, reconstruct the order book in real time, and run deterministic + statistical detection logic inline against live book state to catch wash trading, spoofing/layering, marking-the-close, and front-running — while producing a measurable, honestly-reported precision/recall tradeoff, not just "it caught the bad guy."

---

## 2. Scope

### In scope (v1, full system)
- FIX 4.2 order/execution message simulator and parser (QuickFIX, C++)
- Ingestion layer: lock-free ring buffer (SPSC) feeding into Kafka (librdkafka) for durability/replay
- Hand-rolled order book engine, price-time priority, C++17, updated live from FIX execution reports
- Deterministic rule-based detectors running inline against live book state: WashTrade, SpoofingLayering, MarkingTheClose, FrontRunning
- Statistical baseline detector (z-score) as a control
- Separate Python FastAPI microservice running Isolation Forest for unsupervised anomaly scoring, called out-of-band (not on the hot path)
- TimescaleDB persistence (via libpqxx) — time-series-native storage for order/trade/alert history
- REST API layer (Drogon or Crow) exposing alerts and case data
- React dashboard: live ticker, alert queue, severity-coded alert cards, order book depth visualization, event timeline, compliance action buttons
- Evaluation harness: parameterized synthetic ground truth, precision/recall/F1, threshold sweep, confusion matrix, difficulty-gradient curve
- Offline calibration of synthetic baseline against WRDS/TAQ data (not shipped, not committed — see §7)
- One-off validation against the public CFTC Navinder Sarao spoofing case (see §7)

### Out of scope
- Real exchange connectivity of any kind
- Full multi-venue order routing / smart order routing
- Production-grade auth/entitlements on the dashboard (stub-level only)

---

## 3. Data Model

| Entity | Key Fields | Notes |
|---|---|---|
| **FIX Message (raw)** | msg_type (NewOrderSingle, ExecutionReport, OrderCancelRequest, etc.), all standard FIX 4.2 tags relevant to order lifecycle | Parsed via QuickFIX; normalized into internal Order/Execution structs immediately after parse |
| **Order** | order_id (ClOrdID), account_id, instrument_id, side, price, qty, order_type, timestamp, status, venue | status derived from ExecutionReport OrdStatus tag |
| **Execution** | trade_id (ExecID), order_id, account_id, instrument_id, price, qty, timestamp, counterparty_account_id, venue | |
| **Account/Entity** | account_id, beneficial_owner_id, entity_type, linked_account_ids | Enables wash-trade / front-running detection beyond single-account matching |
| **Instrument** | instrument_id, symbol, asset_class, tick_size, avg_daily_volume, session_close_time | asset_class ∈ {Equity, FX, FixedIncome} |
| **Live OrderBook (in-memory, hand-rolled)** | instrument_id, price levels (bid/ask), FIFO per level, price-time priority | Updated on every relevant FIX execution report; snapshotted for alert evidence |
| **Alert** | alert_id, detector_type, score/severity, account_id(s), instrument_id, evidence, status, ground_truth_label (backtest mode only) | ground_truth_label present only in evaluation runs, never in "live" mode |

---

## 4. Component Architecture

```
FIX Order/Execution Simulator (QuickFIX, C++)
  │  emits FIX 4.2 NewOrderSingle / ExecutionReport / Cancel messages
  ▼
Ingestion Layer
  │  SPSC lock-free ring buffer (in-process hot path)
  │  → librdkafka (durability + replay capability for the evaluation harness)
  ▼
FIX Parsing / Normalization
  │  → internal Order / Execution structs
  ▼
Live Order Book Engine (hand-rolled, price-time priority, C++17)
  │  updated per execution report; per-instrument state
  ▼
Detection Engine — runs inline against live book state
  ├── WashTradeDetector
  ├── SpoofingLayeringDetector      (order lifecycle + live book depth)
  ├── MarkingTheCloseDetector
  ├── FrontRunningDetector
  ├── StatisticalBaselineDetector   (z-score control)
  └── [async, out-of-band] Isolation Forest scoring via Python FastAPI microservice
  ▼
Alert Scoring / Deduplication
  ▼
TimescaleDB (via libpqxx) — order/trade/alert history, time-series native
  ▼
REST API (Drogon or Crow) ──► React Dashboard
                              (live ticker, alert queue, book depth, event timeline)

[Parallel, offline path]
Evaluation Harness ← replays labeled synthetic scenarios through the same pipeline via Kafka replay
  → precision/recall/F1, threshold sweep, confusion matrix, difficulty-gradient curve
  → one-off validation vs. public CFTC Sarao case
```

---

## 5. Key Design Decisions

| Decision | Alternative Considered | Rationale | What's Measured |
|---|---|---|---|
| FIX 4.2 as the native ingestion protocol (QuickFIX) | Flat internal event log, no protocol layer | FIX is a widely recognized keyword across the entire capital markets industry — not just top-tier firms, but mid/small brokerages, prop shops, custodians, and RegTech vendors. Parsing real protocol messages (not a toy format) is a concrete, checkable skill claim with the broadest opportunity surface of any design choice in this project | FIX parsing correctness against known message fixtures |
| SPSC ring buffer feeding Kafka (librdkafka), not either alone | SPSC-only (no durability) or Kafka-only (no ultra-low-latency in-process hot path) | SPSC gives the in-process lock-free hot path (reuses/extends what you proved in the Feed Handler, now applied to FIX message flow); Kafka gives durable, replayable event log needed by the evaluation harness to replay labeled scenarios deterministically | Ingestion throughput; replay determinism (same input → same detector output) |
| Deterministic rule-based detectors run inline against live book state, Isolation Forest kept as a separate, out-of-band microservice | Everything in one C++ process, including ML scoring | Keeps the hot path (book update → rule-based detection) deterministic, testable, and sanitizer-verifiable in pure C++; the ML model gets Python's ecosystem (scikit-learn) without polluting the correctness-critical core, and its async placement is itself a realistic architecture pattern | Rule-based detector latency (must stay on hot path budget); Isolation Forest scored separately, evaluated on its own precision/recall |
| TimescaleDB via libpqxx | SQLite | Time-series-native storage matches the actual shape of this data; libpqxx gives direct C++ Postgres wire-protocol access without an ORM layer hiding query cost | Query correctness + time-range query performance for the dashboard/API |
| React dashboard with dark trading-terminal aesthetic | No UI / API-only | A demoable, screen-shareable interface is a real asset in interviews | N/A (demo asset) |
| Synthetic, parameterized ground-truth data as the primary evaluation dataset, replayed through Kafka for determinism | Real market data only | No public dataset has confirmed abuse labels; synthetic + Kafka replay gives a controllable difficulty gradient AND deterministic re-runs of the exact same scenario through the exact same live pipeline | Precision/recall/F1 as a function of injected severity level |
| WRDS/TAQ used only for offline calibration of the synthetic generator's baseline; never committed to the repo | Ship a dataset derived from TAQ | TAQ is licensed for academic research use, not redistribution; IU affiliation gives access, but nothing derived from it goes in the public repo | Baseline "normal" trading stats checked against published TAQ-derived stylized facts, offline only |
| Single real-world validation: public CFTC Navinder Sarao spoofing case run through the live pipeline as a sanity check | No real-world validation | One publicly confirmed, non-proprietary spoofing case exists and is safe to use | Binary: does SpoofingLayeringDetector fire on the known pattern, at what threshold, through the real pipeline |
| Order book engine hand-rolled rather than using an existing library | Off-the-shelf order book library | Hand-rolling price-time priority matching is the concrete proof of "I understand market microstructure at the implementation level" | Book state correctness under generated FIX sequences; price-time priority invariants hold under test |

---

## 6. Tech Stack

| Layer | Choice |
|---|---|
| FIX protocol | QuickFIX (C++) |
| Ingestion | SPSC lock-free ring buffer + librdkafka |
| Core engine (book, detectors) | C++17 |
| ML anomaly scoring | Python, FastAPI, scikit-learn (Isolation Forest), called async/out-of-band |
| Persistence | TimescaleDB via libpqxx |
| API | Drogon or Crow |
| Dashboard | React (dark trading-terminal aesthetic) |
| Testing | GoogleTest (C++ unit), pytest (Python microservice), custom backtest/evaluation harness |
| Calibration (offline, not shipped) | Python/R against WRDS/TAQ |

---

## 7. Data Sourcing & Compliance Notes (read before building)

- **Primary dataset:** synthetic generator, fully owned, fully shippable, fully labeled, replayed through Kafka for deterministic evaluation runs.
- **WRDS/TAQ access:** IU Libraries provides WRDS access (Kelley School partnership) to IUB-affiliated users, including NYSE TAQ intraday trade/quote data to the microsecond. Register early. **Use for offline calibration only** — never commit raw or derived TAQ data to the repo; confirm exact redistribution terms once registered.
- **CFTC Sarao case:** public enforcement record — safe to reference and safe to run through the pipeline as a validation case.
- **No live exchange connectivity, no real accounts, no real money, no real customer data** anywhere in this system.

---

## 8. The Hard Problem, Stated Explicitly

Two hard problems now, and both need to be verified honestly, not asserted:

1. **Concurrency/ingestion correctness** (shared with the Feed Handler's thesis): SPSC ring buffer correctness under load, Kafka replay determinism, sanitizer-clean under TSan/ASan. Necessary but not the differentiator vs. the Feed Handler — table stakes, done right, not the headline.
2. **Detection-logic correctness** (the actual differentiator): does the live, inline detection pipeline separate abusive from non-abusive behavior at an acceptable false-positive rate, measured via the evaluation harness's threshold sweep and precision/recall curve, and does it still fire correctly when replayed against the one real public case (Sarao)? This is the number that should headline the project.

Bug reports during the build phase should document, for every fix: what the harness measured before, what changed, what it measures after.

---

*Next: phased build guide with explicit "done when" criteria per phase.*
