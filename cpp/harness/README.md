# cpp/harness/ — the evaluation harness: design decisions and real numbers

Companion to `P2_trade_surveillance_engine_architecture.md` and Phase 10 of
the build guide. This is the narrative version: how replay actually reuses
the live pipeline (not a separate offline scorer), what the evaluation
methodology means precisely, and the real precision/recall/F1 numbers —
including the honest, sometimes unflattering ones, per CLAUDE.md's
reporting rule for this phase.

## What "reusing the exact same live pipeline" means here

`replay_runner.cpp` does not reimplement anything from `pipeline/`. It:

1. Converts a `simulator::SimulationOutput` (Phase 1's labeled output) into
   plain `fix::Order`/`fix::Execution` (Phase 2's live-mode structs, which
   have no `ground_truth_label` field at all — the label is stripped by
   construction, not by convention).
2. Publishes them to a real Kafka topic via `KafkaProducer`, flushes, then
   reads them back from the beginning via `KafkaReplayConsumer` (Phase 3's
   deterministic-replay consumer, built for exactly this phase) into a
   fresh `SpscRingBuffer`.
3. Pops that buffer with an unmodified `LiveConsumer`, driving an
   unmodified `LivePipeline` wired with the five Phase 5 rule-based
   detectors — the identical classes `cpp/api/main.cpp`'s production demo
   server uses.

`MlAnomalyDetector` is deliberately excluded: it calls `ml_service/`
asynchronously, and folding a live HTTP round-trip's timing into an
evaluation run would reintroduce exactly the nondeterminism
`KafkaReplayConsumer`'s seek-to-beginning design exists to avoid. Phase
10's own scope (precision/recall/F1, threshold sweep, comparison against
`StatisticalBaselineDetector`) only ever names the five Phase 5 detectors.

`replay_through_kafka()` throws rather than returning a partial result if
the broker is unreachable, if replay doesn't recover every published event,
or if the ring buffer's drop-oldest policy discarded anything — an
evaluation run's numbers are worthless if the replay wasn't complete, so
this fails loud instead of silently reporting on a partial run.

## Evaluation methodology

Ground truth (`ground_truth.hpp`) is built once from the `SimulationOutput`
*before* stripping, indexed by `order_id`/`trade_id` — evaluation-only
bookkeeping, consulted only after alerts come back, never touching the
replay path itself (see `EvidenceTextNeverEmbedsAScenarioId` in
`cpp/tests/harness/replay_runner_kafka_test.cpp`, which checks this
structurally, not just by type).

Each pattern-aware detector's confusion matrix is computed over its own
**evidence universe** — the specific event id space it can possibly
reference, derived directly from which `DetectorEvent` arm/`OrderStatus` it
reacts to (not guessed):

| Detector | Universe | Target pattern |
|---|---|---|
| `WashTradeDetector` | Execution `trade_id`s | WashTrade |
| `SpoofingLayeringDetector` | Cancel-order `order_id`s (it only ever fires from `handle_cancel()`) | SpoofingLayering |
| `MarkingTheCloseDetector` | Execution `trade_id`s | MarkingTheClose |
| `FrontRunningDetector` | New-order `order_id`s | FrontRunning |
| `StatisticalBaselineDetector` | New-order `order_id`s | compared against each pattern separately, plus "any injected abuse" |

TP/FP/FN/TN are all counted in that one universe's unit at a given score
threshold: predicted-positive is the union of `Alert::order_ids` across
every alert from that detector with `score >= threshold`, intersected with
the universe (an id outside it — e.g. `WashTradeDetector`'s
`{execution->order_id, execution->trade_id}` pair, when scored against the
trade_id universe — is silently ignored, not miscounted). `precision()`/
`recall()`/`f1()` return NaN, not 0, when their denominator is zero: "never
fired at this threshold" and "fired perfectly" are different facts.

## Real numbers (main evaluation run)

10 synthetic minutes, 15 instruments, 210 accounts, 15 scenarios per
pattern at severity 0.5, replayed through a real Kafka broker into a real
`LivePipeline`. Full sweep and severity gradient below; nothing here is a
cherry-picked threshold.

```
--- Replay integrity ---
  orders=2423 executions=1567 total_events_published=3990 replayed_from_kafka=3990
  events_processed_by_pipeline=3990 events_skipped_inconsistent=0 ring_buffer_dropped=0

--- Per-detector precision / recall / F1 at operating threshold 0.5 ---
  WashTradeDetector (WashTrade)              TP=30 FP=2  FN=0   TN=1535  P=0.938 R=1.000 F1=0.968
  SpoofingLayeringDetector (SpoofingLayering) TP=5 FP=5  FN=70  TN=264   P=0.500 R=0.067 F1=0.118
  MarkingTheCloseDetector (MarkingTheClose)  TP=0 FP=20 FN=105 TN=1442  P=0.000 R=0.000 F1=n/a
  FrontRunningDetector (FrontRunning)        TP=26 FP=0  FN=19  TN=2034  P=1.000 R=0.578 F1=0.732
```

(`FrontRunningDetector`'s numbers reflect a fixed detector-logic bug, not just re-scored old alerts — see its findings entry below. The other three rows are byte-for-byte identical to the pre-fix run, confirming the fix was isolated to `FrontRunningDetector` alone.)

Full threshold sweeps, the severity-gradient table, and the
baseline-comparison run are reproduced verbatim by running
`./tse_harness_eval` — see "How to run" below; they're not duplicated here
in full to avoid this file drifting out of sync with the actual program.

## Honest findings, one per detector

**WashTradeDetector — works as intended.** F1=0.968, catches all 30
injected wash-trade executions. The 2 false positives are real, not a bug:
baseline flow draws counterparties uniformly from an account pool that
includes the linked pairs, so a baseline execution can by chance land on
two related accounts — genuinely correct detector behavior against noisy
data.

**MarkingTheCloseDetector — a real evidence-contract bug, found and
fixed, plus a scoring-scale caveat.** Before this phase, `check_account()`
in `marking_the_close_detector.cpp` never set `Alert::order_ids` at all —
every fired alert carried `account_ids`/`instrument_id`/window but no
reference to which trades constituted its evidence, violating the
architecture doc's evidence contract and giving this phase a structural 0
true positives regardless of detection quality. Fixed by tracking
contributing `trade_id`s per `(instrument, account)` key
(`trade_ids_by_key_`) and populating `order_ids` at fire time; regression
test: `MarkingTheCloseDetector.AlertOrderIdsContainsEveryContributingTradeId`.

Separately: recall is 0.314 at threshold ≤0.4 but drops to 0.000 at the
uniform 0.5 cutoff used in the headline table above. This isn't a
detection failure — `MarkingTheCloseDetector`'s own `score` is literally
the concentration `share` value, and its own `concentration_threshold`
default is 0.4, so a scenario that just clears the detector's *own* bar
naturally scores in the 0.40–0.49 range. A uniform cross-detector 0.5 cutoff
is too strict for this detector's score scale specifically — exactly why
the full sweep (not the single headline number) is the number that matters.

**SpoofingLayeringDetector — low recall (6.7%, 1 of 15 scenarios caught),
root-caused to a Phase 1/Phase 5 calibration mismatch, not a detector
defect.** Measured directly: of the 15 injected scenarios, only 1 had *any*
layer caught (and that one had all 5 layers caught — this detector's
recall is all-or-nothing per scenario, not partial-credit). Mechanism:
`speed_score` (one of three averaged primary signals) is linear from 1.0 at
0s down to 0.0 at `slow_time_in_book_ns` (default 5s). The scenario
generator's `dwell_ns` (how long a layer rests before being cancelled) is
`lerp(60s, 0.5s, severity)` — solving for when that drops below 5s gives
`severity > 0.924`. Below that, essentially every generated scenario has
`speed_score ≈ 0` regardless of how "obvious" its severity is meant to
make it, handicapping the combined score before `depth_score`/`move_score`
even get a chance to compensate. This is precisely the kind of
generator/detector parameter mismatch Phase 11 exists to close ("Offline
... recalibrate Phase 1's generator parameters against \[real TAQ-derived
cancellation-rate\] stats") — not something patched here.

**FrontRunningDetector — was 0% recall due to a real detector-logic bug
(not a generator/detector semantic disagreement as first suspected),
found via direct instrumentation and fixed.** The initial read was that
the Phase 1 generator and the Phase 5 detector modeled two different,
both-valid front-running sub-patterns. Pushed on that conclusion (a
detector-generator "agree to disagree" is a much bigger claim than a bug,
and deserved more scrutiny before being accepted), a closer look at the
standard/regulatory definition — trading ahead of a *known, pending* order
using advance knowledge that it exists, not requiring knowledge of a
future order that hasn't been placed yet — settled it: the generator is
correct. `abuse/front_running.cpp`'s own comment says exactly this ("how
quickly the related account trades after the client order is placed"):
the client's large order is placed first, becoming known; the related
account reacts 20–150ms later and gets filled well before the client's own
much-later fill. That *is* front-running by the textbook definition.

The bug was in `front_running_detector.cpp`: it required the smaller
order to precede the large one (`leader.timestamp_ns > order.timestamp_ns
→ continue`), which structurally could never match what the generator
(correctly) produces. Fixed by swapping which side is the loop's trigger
and which is its history: a large order is now recorded when it arrives,
and a smaller related order arriving shortly *after* it (within
`lookback_window_ns`, still size-ratio-gated) is what fires the alert —
mirroring `SpoofingLayeringDetector`'s own established "skip a
causality-inverted pairing, don't destroy it" pattern for handling
ordinary cross-account clock skew, just with the roles reversed. All 13
tests in `front_running_detector_test.cpp` were rewritten for the
corrected direction (including both non-monotonic-entry regressions,
re-derived under the new roles) and pass; the Kafka integration test's
former special-case for this detector was removed since it now fires
genuinely, like the other three.

Net effect on this run: TP 0→26, recall 0.000→0.578, precision 1.000 (was
undefined). The remaining 19 false negatives were checked individually,
not assumed — every one is accounted for, and none of them is a detector
defect:

- **15 of 19** are the scenario's `reversal_order` (the extra unwind trade
  `abuse/front_running.cpp` adds when severity ≥ 0.5, on the *opposite*
  side, placed *after* the client's fill). It lands in this evaluation's
  New-order universe because it shares the scenario's `ground_truth_label`,
  but it's structurally outside what this detector targets — order-
  placement sequencing ahead of a *pending* order, not a post-fill unwind
  — so it was never reachable regardless of the fix. This is a universe-
  construction property of this evaluation (any New order carrying the
  label counts as "positive"), not a detector gap.
- **4 of 19** (2 full scenario pairs, `SCN-FR-000009` and `SCN-FR-000011`)
  are genuine, correct rejections: the related account's quantity (300 and
  500) exceeded `max_leader_to_large_size_ratio` (0.2) relative to the
  client's (1200 and 1800) — 0.25 and 0.278 respectively — by chance draws
  from the generator's independent `qty_client`/`qty_related` ranges. The
  detector's own size-ratio gate is doing exactly its documented job here
  (excluding a pair that isn't "a small position relative to the large
  order"), not failing.

The threshold sweep table below is flat at R=0.578 across every threshold
(this detector's score is always exactly 1.0 when it fires — a
deterministic rule, not a heuristic, matching its own class comment), so
none of the 19 is a threshold-calibration artifact either.

**StatisticalBaselineDetector — 0% recall in the main run, but for a
methodological reason, not a detector weakness: it almost never fires at
all.** It needs `min_sample_count` (default 5) *prior* New orders on the
exact same `(account_id, instrument_id)` key before it can compute a
z-score. Measured directly against the main run's config: only ~1.7% of
the 1,448 distinct keys ever reach 5 observations in a 10-minute session
across 210 accounts × 15 instruments, and only 6.7% of the abuse-scenario
orders themselves land on a key with enough history. A second, much
smaller-cardinality run (25 accounts, 3 instruments — see
`baseline_comparison_config()`) gives it a fair chance to warm up; there,
`WashTradeDetector` still beats it decisively (R=1.000 vs R=0.100,
P=0.667 vs P=0.059), which is the actual "how much better than naive"
answer for the one detector Phase 1/5's calibration doesn't currently
handicap. `FrontRunningDetector` now beats naive baseline just as
decisively (R=0.556 vs R=0.111, P=0.893 vs P=0.098) since its logic fix
above. `SpoofingLayeringDetector` still loses to the naive baseline *in
that comparison run* — an honest result, not omitted: it's still
suppressed by the severity/dwell-time calibration issue above, while the
baseline's qty-based z-score picks up the abuse scenarios' unusually large
order sizes directly, independent of timing semantics.

## Difficulty gradient (severity 0.1 → 0.9, recall at threshold 0.5)

```
severity     WashTrade  SpoofingLayering  MarkingTheClose  FrontRunning
0.1          1.000      0.000             0.000            0.800
0.3          1.000      0.000             0.000            0.700
0.5          1.000      0.240             0.000            0.467
0.7          1.000      0.300             0.000            0.533
0.9          1.000      0.017             0.000            0.533
```

`FrontRunningDetector` is high and roughly flat (0.47–0.80) across the
whole range, unlike the other two heuristic-affected detectors — expected,
since neither of its two real miss causes (the structurally-out-of-scope
`reversal_order`, and the size-ratio gate) depends on severity at all;
`qty_client`/`qty_related` are drawn from fixed ranges regardless of
severity. Its mild, non-monotonic wobble should be read as noise, not a
trend, for the same reason given for `SpoofingLayeringDetector` below,
plus one more: `severity_gradient_config()` runs all four abuse patterns
at the same severity in one simulation, and severity changes how many
random-number draws the *other three* patterns' generators consume before
`FrontRunningDetector`'s own 10 scenarios are drawn (e.g.
`generate_front_running_scenario` itself branches on `severity >= 0.5` for
the `reversal_order`) — shifting the RNG state shifts which accounts/
instruments/timestamps every later scenario lands on, run to run. Combined
with only 10 scenarios per point, this curve is a noisy sample, not a
smooth function of severity.

`WashTradeDetector` is severity-invariant (it's a deterministic relation
check, not a heuristic — matches its own class comment).
`MarkingTheCloseDetector` stays at 0.000 across the whole range at this
fixed 0.5 threshold for the scoring-scale reason above, independent of
severity. `SpoofingLayeringDetector`'s non-monotonic curve (peaks at 0.7,
drops at 0.9) is consistent with the `severity > 0.924` calibration
threshold derived above: none of these five sample points cross it, so the
small nonzero values are `dwell_ns`'s built-in jitter occasionally landing
under 5s by chance, not a real trend — each point is only 10 scenarios, so
this curve is noisy by construction and shouldn't be read as a smooth
function.

## How to run

```bash
docker compose up -d kafka   # KafkaReplayConsumer needs a real broker
cmake --build build-bench --target tse_harness_eval
./build-bench/cpp/harness/tse_harness_eval
```

Prints the full report (replay integrity, per-detector confusion matrices,
the baseline comparison, the complete threshold sweep, and the severity
gradient) to stdout. Takes roughly 20–30s (one main replay + one baseline-
comparison replay + five severity-gradient replays, each a real Kafka
round trip).

## Tests

`cpp/tests/harness/evaluation_test.cpp` — pure confusion-matrix/threshold-
sweep/universe-building math, no Kafka needed.
`cpp/tests/harness/ground_truth_test.cpp` — index construction.
`cpp/tests/harness/replay_runner_kafka_test.cpp` — real Kafka integration:
replays an obviously-injected (severity 0.9) simulation and proves every
one of the four pattern-aware detectors catches at least one of its own
scenarios with correctly-attributed evidence; also structurally proves no
evidence text ever embeds a ground-truth `scenario_id`. Skips cleanly (not
a failure) if Kafka isn't reachable, matching
`cpp/tests/ingestion/kafka_replay_test.cpp`'s established pattern.

## Two other real bugs found while verifying this phase's numbers

Neither is `harness/`'s own code — one surfaced from ordinary live
dashboard use, the other from re-running the full sanitizer suite as part
of confirming this phase's fix didn't regress anything else — but both
were found during this phase's verification pass and are documented in
full in their owning modules, summarized here for continuity:

- **`AlertStore` transaction-concurrency crash** (`cpp/db/`): a single,
  unsynchronized `pqxx::connection` shared between `api/`'s multithreaded
  Crow handlers and `pipeline/`'s consumer thread crashed the live demo
  server under ordinary concurrent access
  (`pqxx::usage_error: Started new transaction while transaction was still
  active`). Fixed with a mutex around every `AlertStore` method; see
  `cpp/db/README.md` and `ConcurrentAlertStoreTest`.
- **TSan latency-budget miscalibration** (`ml_client/`):
  `GracefulDegradation.HotPathLatencyStaysWithinBudgetRegardlessOfServiceState`'s
  1ms/5ms budget was set against a no-sanitizer build (Phase 6) and never
  revisited for TSan's own instrumentation overhead, which consistently
  inflated measured latency to ~2.2–2.4x that budget even with all other
  load stopped. Fixed with a TSan-specific (looser, still meaningful)
  budget; see `cpp/ml_client/README.md`.
