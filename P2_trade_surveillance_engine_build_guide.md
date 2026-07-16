# FIX-Native Trade Surveillance Engine — Step-by-Step Build Guide

Same process as the Feed Handler: build one verified piece at a time, correctness before concurrency, don't move to the next phase until the "Done when" criterion is actually true — not "it ran once."

12 phases total (0–11).

---

## Phase 0 — Project scaffolding

- [ ] CMake project, two configs: `benchmark` (optimized, no sanitizers) and `debug` (ASan + TSan enabled)
- [ ] GoogleTest set up for C++ unit tests
- [ ] Folder structure: `fix/`, `ingestion/`, `orderbook/`, `detectors/`, `harness/`, `db/`, `api/`
- [ ] Separate `ml_service/` directory for the Python FastAPI microservice
- [ ] `dashboard/` directory for the React app
- [ ] Docker Compose file bringing up TimescaleDB and Kafka locally

**Done when:** `cmake --build` succeeds in both configs with a trivial passing test; `docker compose up` brings up TimescaleDB + Kafka; the Python service and React app both scaffold-run.

---

## Phase 1 — Order simulator (synthetic generator)

- [ ] Parameterized generator producing realistic baseline order flow (multi-asset: Equity, FX, FixedIncome instruments)
- [ ] Injected abuse patterns: wash trading, spoofing/layering, marking-the-close, front-running — each with a **severity/subtlety parameter**
- [ ] Every generated event carries a `ground_truth_label` (strippable — "live mode" output must look identical to real order flow)
- [ ] Output format: internal Order/Execution structs, serializable to FIX text (feeds Phase 2) and to a labeled JSON/CSV (feeds Phase 10's harness)
- [ ] Unit tests: generator produces the expected *rate* of each abuse pattern at a given severity setting

**Done when:** running the generator at severity=low vs severity=high visibly changes how "obvious" injected patterns are (spot-check manually), and ground-truth labels are 100% recoverable and strippable.

---

## Phase 2 — FIX session layer (QuickFIX, C++)

- [ ] QuickFIX integration: parse `NewOrderSingle`, `ExecutionReport`, `OrderCancelRequest` (FIX 4.2)
- [ ] Session-level handling: sequence numbers, heartbeats, basic session recovery
- [ ] Round-trip test: internal Order struct → FIX message → parsed back → equal to original
- [ ] Malformed/out-of-sequence message handling — must not crash the session

**Done when:** all three message types round-trip correctly under test, sequence-number gap and out-of-order delivery are handled without crashing, and the simulator (Phase 1) can drive a real FIX session end-to-end into the parser.

---

## Phase 3 — Ingestion queue

- [ ] Build the **SPSC lock-free ring buffer first, single-threaded correctness test only**
- [ ] Add the second thread (producer = FIX parser, consumer = order book updater); verify under **ThreadSanitizer specifically**
- [ ] Add `librdkafka` as the durable/replayable layer sitting behind the ring buffer — this is what makes Phase 10's deterministic replay possible
- [ ] Backpressure policy defined and tested (drop-oldest, block, or grow — pick one and justify it)

**Done when:** the ring buffer passes under TSan with sustained producer/consumer load, Kafka replay of a recorded message sequence reproduces bit-identical downstream state, and the backpressure policy is exercised by a test that deliberately stalls the consumer.

---

## Phase 4 — Order book engine

- [ ] Hand-rolled book: price-time priority, correct handling of add/cancel/execute/replace
- [ ] **Single-threaded first** — full correctness proven before this is wired to live ingestion
- [ ] Unit tests against known sequences with hand-computed expected book state (this is the phase that gets the deepest interview scrutiny — don't under-test it)
- [ ] Depth snapshot capability (needed by Phase 5's spoofing detector and Phase 8's alert evidence)

**Done when:** book state is correct under a suite of hand-verified sequences including edge cases (order at exact same price/time, partial fills, cancel-after-partial-fill), and depth snapshots at any point in time can be reconstructed and are provably consistent with the update sequence that produced them.

---

## Phase 5 — Detection layer (deterministic rules)

Build and unit-test each detector **in isolation, against static book/order data**, before wiring any of them to live streaming state.

- [ ] `WashTradeDetector` — same-beneficial-owner or linked-account both-sides matching
- [ ] `SpoofingLayeringDetector` — order lifecycle + live book depth (time-in-book, % of visible depth, cancel timing relative to price moves) — **this is the hardest detector; budget the most time here**
- [ ] `MarkingTheCloseDetector` — concentrated activity near session close
- [ ] `FrontRunningDetector` — related-account sequencing ahead of client flow
- [ ] `StatisticalBaselineDetector` — z-score control, used later as the comparison point in the evaluation harness
- [ ] Each detector exposed through a common interface so the harness can swap/compare them uniformly

**Done when:** each detector independently passes unit tests against hand-constructed scenarios (both true-positive and true-negative cases you wrote yourself, before ever touching the synthetic generator's labels).

---

## Phase 6 — Live integration (ingestion → book → detectors)

- [ ] Wire Phases 3, 4, and 5 together: FIX flow → ring buffer/Kafka → live order book → detectors running inline against live book state
- [ ] Full pipeline TSan/ASan run under sustained load
- [ ] Latency budget check: rule-based detection must stay on the hot path without materially slowing book updates (measure it, don't assume it)

**Done when:** the full C++ pipeline runs end-to-end from simulated FIX flow to live alerts, sanitizer-clean under load, with measured (not assumed) detector latency on the hot path.

---

## Phase 7 — ML anomaly microservice (Python, out-of-band)

- [ ] FastAPI service, Isolation Forest (scikit-learn) trained on volume/frequency features
- [ ] Called from C++ via REST/gRPC, **async, off the hot path**
- [ ] Failure handling: what happens to the pipeline if the ML service is slow or down (must degrade gracefully)
- [ ] pytest suite for the service itself, independent of the C++ pipeline

**Done when:** the microservice runs independently and passes its own tests, the C++ side calls it asynchronously with a proven-graceful fallback when it's unavailable, and you can demonstrate the hot path is unaffected by artificially slowing the ML service down.

---

## Phase 8 — Alert store (TimescaleDB)

- [ ] Schema for orders/trades/alerts, time-series-native design (hypertables where appropriate)
- [ ] libpqxx integration from C++
- [ ] Alerts written with detection type, confidence score, and evidence
- [ ] Query correctness tests: time-range queries, filter-by-account, filter-by-detector-type

**Done when:** alerts generated in Phase 6/7 land correctly in TimescaleDB with full evidence, and a set of representative queries return correct, tested results.

---

## Phase 9 — API + dashboard

- [ ] REST API (Drogon or Crow): alert listing/filtering, order book snapshot retrieval, case status endpoints
- [ ] React dashboard: live ticker, alert queue, severity-coded alert cards, order book depth visualization, event timeline, compliance action buttons
- [ ] Dashboard reads real data from the API — no mocked/hardcoded frontend data

**Done when:** the dashboard, running against the live pipeline, correctly displays alerts as they're generated, with working filters and a depth visualization that matches actual book state.

---

## Phase 10 — Evaluation harness

- [ ] Replay labeled synthetic scenarios (Phase 1 output) through Kafka into the **exact same live pipeline** used in production mode
- [ ] Compute precision/recall/F1 per detector
- [ ] Threshold sweep + confusion matrix per detector
- [ ] Difficulty-gradient curve: detector performance as a function of the severity parameter from Phase 1
- [ ] Explicit comparison: pattern-aware detectors vs. `StatisticalBaselineDetector`

**Done when:** you have real, reproducible precision/recall/F1 numbers per detector, a threshold sweep chart, and a clear quantified answer to "how much better than a naive z-score is your spoofing detector."

---

## Phase 11 — WRDS/TAQ calibration + CFTC Sarao validation

- [ ] Offline (not shipped, not in repo): pull baseline stylized facts from WRDS/TAQ — volume clustering, spread behavior, cancellation rates
- [ ] Recalibrate Phase 1's generator parameters against these real stats
- [ ] Re-run Phase 10's evaluation harness with the recalibrated generator; document whether/how numbers changed
- [ ] Construct the CFTC Sarao case as a FIX-replayable scenario and run it through the **full live pipeline** — record whether `SpoofingLayeringDetector` fires, and at what threshold

**Done when:** you can show (privately, from your own notes — not committed data) that the synthetic baseline is grounded in real TAQ-derived statistics, and you have a documented, honest result for the Sarao validation run, including if it *doesn't* fire cleanly.

---

## What NOT to do

- Don't wire live concurrency (Phase 6) before Phases 3, 4, and 5 are each independently correct.
- Don't skip the TSan run on the ring buffer/live integration.
- Don't let the ML microservice (Phase 7) sit on the hot path, even "temporarily to test it."
- Don't commit anything derived from WRDS/TAQ to the repo (Phase 11).
- Don't report Phase 10/11 results by cherry-picking the best threshold — report the full sweep.

---

## If time runs short later

Cut in this order, protecting the phases that carry the real interview story: **dashboard (Phase 9) first, then ML microservice (Phase 7), then Kafka (fall back to SPSC-only)** — in that order. None of those three touch the order book (Phase 4) or the detection-correctness numbers (Phases 5, 10, 11). A working pipeline with alerts in TimescaleDB and no dashboard is a complete, defensible project. A polished dashboard over unverified detection logic is not. FIX (Phase 2) should be the last thing cut, if ever — it's the phase this project is named for and the biggest driver of opportunity breadth across mid/small-tier companies.

---

## How to proceed

Work through Claude Code one phase at a time, in order. For each phase:

1. Take the phase's checklist into Claude Code as your task list.
2. Build it, run the tests/sanitizers specified in that phase's "Done when" criterion.
3. Paste the phase report back here — what you built, what you tested, any bugs found and how you fixed them, and the actual output of whatever verification that phase requires.
4. I'll check it against that phase's "Done when" criterion specifically before you move to the next phase.

Ready to start with Phase 0 whenever you are.
