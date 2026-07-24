# cpp/pipeline/ — live wiring: design decisions and measured latency

Companion to each class's own header comments. This is the narrative
version: why the module exists at all, the three real bugs its own tests
surfaced — one of them in Phase 3's ring buffer, found only once this
phase's more adversarial sustained-load test existed to find it — and the
actual measured latency numbers the build guide's Phase 6 "Done when"
criterion asks for.

## Why a new module, when the architecture doc didn't list one

The documented repo layout had `fix/`, `ingestion/`, `orderbook/`,
`detectors/` as siblings with no module that actually wires them together
for live use. Implementing Phase 6 surfaced that this glue needs its own
home: per-instrument `OrderBook` routing was explicitly deferred out of
Phase 4 ("Phase 6's live-pipeline wiring concern, not the book engine's"),
and the architecture doc's own stated goal for Phase 10 — "replay mode
reuses the *exact same* detection code path as live mode" — only holds if
that wiring lives somewhere reusable rather than being duplicated between a
live driver and a replay driver. `cpp/pipeline/` is that home. See
`P2_trade_surveillance_engine_architecture.md` §1–§4, updated this phase.

## `IEventSink`: fix/ owns the abstraction, pipeline/ implements it

`SurveillanceFixApplication` (Phase 2) needed a hook to hand parsed
Order/Execution structs to the ring buffer, but `fix/` depending on
`ingestion/` directly would invert the architecture doc's dependency table
(`ingestion/` depends on `fix/`, never the reverse). The fix: `fix/`
declares `IEventSink` (`event_sink.hpp`) as its own abstraction;
`pipeline/`'s `PipelineEventSink` is the concrete implementation that
bridges into `ingestion/`. `set_event_sink()` is purely additive —
existing Phase 2 tests that never call it see byte-identical behavior to
before this phase (verified: all 18 `fix_tests`, including four pre-existing
`LiveSessionFixture` tests, still pass unmodified).

## Three real bugs, all caught by actually running the sustained-load test

**Bug 1 — the first version of the 100k-event test measured almost
nothing.** An unpaced producer (raw struct copies into the ring buffer)
overwhelms a consumer doing real `OrderBook::apply()` plus five
`IDetector::evaluate()` calls per event so completely that of 204,814
pushed events, only 8,408 were ever processed — a 96% drop rate. The
"sustained load" and "measure real latency" claims would have been resting
on 4% of the intended scale. Fixed by adding a second producer mode, paced
against the consumer's progress (`while (queue.size_approx() >
capacity/2) yield();`), mirroring Phase 3's own
`SustainedLoadWithConsumerKeepingUpDropsNothing` test. With pacing, all
204,814 events are processed with zero drops — this is the run the latency
numbers below come from. The original unpaced version is kept as a
*second*, deliberately adversarial test
(`UnpacedBurstUnderSevereBackpressureNeverCrashesAndAccountingHolds`) —
its job isn't latency measurement, it's proving the drop-induced-
inconsistency handling (next section) is safe under real, severe
backpressure with a real five-detector workload, not just the
hand-constructed single-event case in `live_pipeline_test.cpp`.

**Bug 2 — a genuine O(n) hot-path issue in Phase 5's
`SpoofingLayeringDetector`, only visible under real sustained load.**
`count_concurrent_same_account_same_side()` did a full linear scan over
*every* currently-tracked order (all accounts, all instruments) on *every*
single Cancel — fine at Phase 5's hand-constructed test scale (a handful of
orders), but `tracked_` can hold thousands of entries across a multi-hour,
multi-instrument synthetic session, and every one of them was being
examined regardless of relevance. This didn't show up as a correctness bug
(Phase 5's 12 tests still passed, and still pass) — it showed up as a heavy
latency tail once real volume flowed through: before the fix, detector
mean latency was 9,052ns with a p99 of 207,417ns; after replacing the scan
with an incrementally-maintained `concurrent_count_by_key_` index (kept in
sync via a single `erase_tracked()` helper that every insertion/removal
path now goes through — see the class's updated header comment), mean
dropped to 179ns (~50x) and p99 to 459ns (~450x). This is exactly what
"measure it, don't assume it" is for: the bug was invisible without a real
sustained-load run, and the fix is verified by a clean before/after
comparison, not asserted.

**Bug 3 — a genuine TSan-caught data race in Phase 3's `SpscRingBuffer`
itself, the most significant finding this phase.** Once Bug 1 was fixed
(the paced test getting real throughput) and the *unpaced* burst test was
run under TSan at the volume this phase's real book-plus-five-detector
workload produces (~150,000+ drop-oldest reclaims in one run — far more
sustained churn than Phase 3's own tests exercised), TSan reported a
genuine data race between `push()`'s write of a new element and
`try_claim_front()`'s speculative pre-CAS read — the exact two lines
Phase 3's own "Bug 2" fix touched. That fix (read a copy before attempting
the CAS, not after) was correct for the race it targeted, but left a
second window open: a *losing* CAS attempt's read doesn't synchronize with
a *different*, winning thread's *later* write to that same physical slot
after a wraparound, because a failed `compare_exchange_weak` only provides
`memory_order_relaxed` on failure. Phase 3's tests never ran long or hard
enough, with real enough data, to hit that window; Phase 6's did,
immediately. Fixed by replacing the copy-before-CAS scheme with per-slot
sequence numbers (Vyukov's bounded-queue technique) — full derivation in
`cpp/ingestion/spsc_ring_buffer.hpp`'s class comment and
`cpp/ingestion/README.md`'s "Bug 3" section, since this is a Phase 3 class
being fixed, not new Phase 6 code. Re-verified after the fix: all 180
tests clean across fresh benchmark/ASan/TSan builds (not incremental —
`rm -rf` and reconfigure each time), plus 5 repeated clean TSan runs of
the specific test that found it, matching the rigor Phase 3 itself applied
to its own first two bugs. This is the clearest instance in the whole
project so far of "TSan-clean under the tests you wrote" not being the
same claim as "race-free" — a harder, more realistic test is what it took
to find this, and building that harder test was itself part of what Phase
6 asked for.

## Drop-induced inconsistency: an accepted consequence, not a crash

Under drop-oldest backpressure, a *logically* inconsistent event can reach
`OrderBook` — e.g. an Execution or Replace referencing an order_id whose
New the ring buffer dropped. Phase 4's `OrderBook::apply()` is designed to
throw `std::invalid_argument` for exactly this ("a genuine invariant
violation," reasonable in Phase 4's single-threaded, hand-constructed-
sequence world). Under real sustained load with drop-oldest active, it can
also legitimately mean "the prerequisite event was dropped" — an accepted,
already-documented consequence of Phase 3's policy, not a defect.
`LivePipeline::process()` catches `std::invalid_argument` specifically
(not a blanket catch) around the book-apply step, counts it via
`inconsistent_events_skipped()`, and skips detector evaluation for that
event rather than propagating and killing the consumer thread. Tested at
two scales: a single hand-constructed case in `live_pipeline_test.cpp`
(`ExecutionForNeverSeenOrderIsSkippedNotThrown`), and genuinely, at volume,
in the unpaced-burst sustained-load test (1,415–1,513 real occurrences
across runs, all handled cleanly).

## Latency — the actual measured numbers

From `LiveConsumerSustainedLoad.ConsumerKeepingUpProcessesNearlyEveryEventAndReportsLatency`
(204,814 events, 3 equity + 2 FX + 2 fixed-income instruments, 40
independent + 8 linked-pair accounts, wash-trade/spoofing/marking-the-
close/front-running patterns injected, all 5 real detectors registered,
zero drops):

All three from fresh builds, same 204,814-event scenario, zero drops, 762
alerts, all-tests-pass in every config — the numbers below are what
actually came out of each run, not extrapolated from one:

| | book_apply_ns mean | p50 | p99 | max | detectors_ns mean | p50 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| **benchmark** | 48.8 | 42 | 167 | 84,792 | 179.3 | 166 | 542 | 654,959 |
| **ASan** | 1,316.5 | 1,292 | 2,750 | 720,667 | 4,832.2 | 4,708 | 8,792 | 2,579,750 |
| **TSan** | 3,568.2 | 3,708 | 6,000 | 632,167 | 13,894.9 | 13,750 | 22,875 | 5,022,042 |

Detector mean latency stays at roughly 3.6–3.9x book-apply mean latency
across all three configs — the ratio is stable under sanitizer
instrumentation, not just at benchmark speed — comfortably within the
budget asserted in the test (mean detector latency must stay under
`20 * mean_book_ns + 1000ns`, and under 1ms outright; p99 under 5ms — TSan's
own p99 of 22.9µs is nearly 220x under that budget). Sanitizer
instrumentation adds real, expected overhead (ASan ~27x, TSan ~78x on
detector mean vs. benchmark) but doesn't change the qualitative picture:
zero drops, all five detectors run, real alerts fire, latency stays low
and the tail stays proportionate — in every config, not just the fast one.

The occasional high max (both book_apply_ns and detectors_ns show outliers
far above their own p99) is consistent with ordinary scheduling/cache
jitter on a shared machine, not a systematic pattern — p99 being tight and
close to the median for both is what actually distinguishes "rare outlier"
from "the real O(n) tail Bug 2 produced," which is exactly what changed
between the before/after numbers above.

## Verification

`pipeline_tests`: 8 tests (6 `LivePipeline` single-threaded correctness, 2
`LiveConsumerSustainedLoad`). `fix_tests`: 18 tests, including the new
`DrivesSimulatedFixFlowThroughTheRingBufferIntoTheLivePipeline` (real
acceptor+initiator TCP session, ~2,100 messages, `IEventSink` wired end to
end into a real `LiveConsumer`/`LivePipeline`, zero drops, alerts fired) —
deliberately moderate scale, not 100k+: driving that many individual
messages through real per-message socket round trips under TSan would take
a long time without adding proof beyond what this test and the
ring-buffer-scale sustained-load test separately already cover; QuickFIX's
own transport thread-safety was already proven at scale in Phase 2, and
what's genuinely new this phase is the `IEventSink` handoff specifically.
`detectors_tests`: unchanged pass count (12/12 `SpoofingLayeringDetector`,
confirming the O(1) concurrency-index refactor didn't change any observable
behavior, only its cost).

`ingestion_tests`: all 19 non-Kafka Phase 3 tests still pass unmodified
after the Bug 3 ring-buffer rewrite, including
`TwoThreadPipeline.SustainedLoadPreservesOrderingWithNoLossOrCorruption`
(the original test that forced Bugs 1 and 2 in Phase 3) — confirming the
rewrite is a genuine drop-in replacement, not a behavior change dressed up
as a bug fix.

Full project suite: 180/180 tests passing, from completely fresh
(`rm -rf` + reconfigure) builds, in all three configs — benchmark, ASan,
and TSan — the last of these run with `tsan_suppressions.txt` active for
the confirmed QuickFIX-internal races (see `cpp/fix/README.md`) so real
findings in this project's own code, like Bug 3, aren't masked by
third-party noise. Zero TSan warnings anywhere in the final run's full
captured log (`grep -c "WARNING: ThreadSanitizer"` → 0), plus 5 additional
repeated clean runs of the specific test that originally caught Bug 3.

## Alert-persistence stall investigation (Phase 12) — a real bug found, the original trigger unresolved

Discovered indirectly: Phase 12's KPI strip showed `ML SERVICE: HEALTHY`
and real `MlAnomalyDetector` alerts, but a live screenshot review noticed
the alert queue was *entirely* `MlAnomalyDetector`, all clustered in a
narrow score band — worth investigating before trusting it, not
dismissing as "ML is just active right now."

**What was actually observed, confirmed via direct queries against the
real alert store, not inferred from the UI.** A `tse_api_server` process
running 7h23m had its *newest* alert — across all six detectors,
including `MlAnomalyDetector` — sitting at ~7.2 hours old
(`time.time_ns() - alert.window_start_ns`, both real epoch-ns, no
timezone ambiguity). A live 30-second wait produced zero new alerts, from
any detector. Meanwhile `GET /api/orderbook/ACME/snapshot`'s `sequence`
kept climbing the entire time (confirmed by direct repeated polling) —
the live pipeline itself was demonstrably healthy; only alert persistence
had gone permanently silent, roughly 14 minutes into that process's life,
with no crash.

**Every plausible mechanism was tested directly, not assumed:**

- **Consumer thread death.** Ruled out immediately: `OrderBook::apply()`
  and detector `evaluate()` happen sequentially in the same
  `LivePipeline::process()` call (see above) — if the consumer thread had
  died, book state would have frozen too. It never did, in any observed
  run, stalled or healthy.
- **`DbAlertSink::on_alert()` throwing silently.** `on_alert()` has no
  try/catch by design (`db_alert_sink.hpp`) — an uncaught exception
  escaping the consumer thread's entry point calls `std::terminate()`,
  crashing the whole process. It never crashed, in any run. Temporarily
  wrapped `on_alert()` in a logging try/catch/rethrow to confirm directly:
  across a 25-minute healthy run, zero throws logged.
- **A genuine data race.** Rebuilt `tse_api_server` under TSan
  (`build-tsan`), removed the demo feeder's 15ms pacing entirely to force
  maximum throughput (same idea as this file's own sustained-load tests),
  and ran it for 25+ minutes across two separate attempts, totaling
  3.5M+ processed events. TSan found 3 races — all the same one,
  confirmed third-party: `crow::CerrLogHandler::log()` racing on
  `std::cerr`'s internal `ios_base::width` state across concurrent HTTP
  handler threads (Crow's own multithreaded request logging, not this
  project's code). Zero races found anywhere in `LivePipeline`,
  `DbAlertSink`, `LiveBookRegistry`, any detector, or `MlScoringWorker` —
  consistent with the architecture: `MlScoringWorker` runs on its own
  thread but never touches detector state, only the shared `alert_sink_`
  (confirmed properly mutex-guarded by reading `AlertStore::insert_alert()`
  directly — takes its lock as the first line).
- **Ring-buffer drop-oldest backpressure starving every detector before
  dispatch.** The one theory that could explain a *stateless* detector
  (`WashTradeDetector` has no member variables at all) going silent
  alongside the stateful ones. Wired `SpscRingBuffer::dropped_count()`
  through `LiveBookRegistry` and logged it alongside
  `events_skipped_inconsistent()` every 5,000 events. Under forced max
  throughput, `skipped_inconsistent/events_processed` climbed but
  **decelerated** (53.2%→62.9% over 25 minutes, delta shrinking each
  sample) — converging, not spiking. Every detector except
  `MarkingTheCloseDetector` (frozen at 36 — its already-documented,
  by-design lifetime-dedup exhaustion, not a bug) kept incrementing its
  alert count on every single sample across 3.2M+ events. Ruled out.

**One real, confirmed bug found and fixed along the way, independent of
whether it's THE cause.** `SpoofingLayeringDetector::AmbientTracker`'s
prune step was a front-only trim (`while event_ts - front > window, pop`)
that silently assumes `event_ts` is monotonically non-decreasing across
calls. `cpp/api/main.cpp`'s demo feeder reuses the same fixed
`session_start_ns` anchor for every session in its infinite loop (by
design, to keep `MarkingTheCloseDetector`'s close-time config valid
across loops) — so the live `event_ts` stream genuinely regresses
backward by up to 90 synthetic seconds at every session boundary, for the
life of the process. Under the old trim, a regressed `event_ts` compared
against a deque whose front is a much-later timestamp never satisfies the
trim condition (the subtraction goes negative) — stale entries from a
"future" session's perspective get stuck forever, inflating
`recent_move_rate` and suppressing `move_score`. `FrontRunningDetector`'s
analogous `recent_by_key_` structure was already immune (symmetric
`std::llabs()` + full-scan `remove_if`, not a front-only assumption) —
this was a genuine asymmetry in the codebase, not a hypothetical. Fixed
by giving `AmbientTracker` the same symmetric pruning. Verified as a real
regression, not just "the current implementation's behavior": temporarily
reverted the fix, ran the new test
(`AmbientPruneSurvivesATimestampRegressionAcrossASessionBoundary`) against
the old code — it failed with a measurably wrong score (0.878 vs. the
correct 83/90 ≈ 0.922) — then restored the fix and confirmed it passes.
17/17 `SpoofingLayeringDetector` tests clean.

**Honest conclusion: the original stall was never reproduced under
controlled conditions, despite five separate attempts.** Of five runs —
the original (naturally paced, real-world), a fresh restart at the same
pacing, a fresh restart with debug instrumentation at the same pacing,
and two TSan runs at forced maximum throughput (3.2M+ and 538K+ events
respectively) — only the *original* process ever showed the stall. Every
deliberate reproduction attempt since, including under far higher event
volume and load than the original needed to fail, came back clean. This
rules out "sustained load" or "long runtime" alone as a sufficient
trigger; whatever caused it is rarer and more specific — possibly
dependent on something about that process's exact history that hasn't
been replicated (a one-time TimescaleDB/Docker blip during a long idle
gap, some interaction with the many other `tse_api_server`
restarts/rebuilds happening elsewhere that same session, or a race too
rare to hit in ~4M cumulative events of deliberate hunting). The
`AmbientTracker` fix above is real and worth having regardless, but it is
**not confirmed as the original stall's cause** — it was found via code
review during the search, not by reproducing the stall and observing it
resolve.

If this recurs: the debug instrumentation pattern used here is cheap to
redeploy and already proved useful for bisection —
(1) a heartbeat in `LivePipeline::process()`'s detector loop counting
alerts found per detector index, (2) a logging try/catch/rethrow wrapper
around `DbAlertSink::on_alert()`, (3) `SpscRingBuffer::dropped_count()`
piped through `LiveBookRegistry` and logged alongside
`events_skipped_inconsistent()`. All three were removed after this
investigation (kept the codebase clean per CLAUDE.md's no-speculative-
scaffolding standard) but the exact shape is preserved here, not just "we
looked into it once."

## The session-boundary regression bug, found a third and fourth time (Phase 12)

The same root cause as `AmbientTracker` above — `cpp/api/main.cpp`'s demo
feed loop reusing one fixed `session_start_ns` anchor for every session,
so live event timestamps genuinely regress backward at every session
boundary — turned out to have two more independent victims, found while
investigating two *different* live-dashboard symptoms, neither of which
was itself a stall:

- **`MlAnomalyDetector::handle_order()`'s tumbling window** (`ml_client/`,
  documented in full in `cpp/ml_client/README.md`'s addendum): the same
  front-only-assumption shape as `AmbientTracker`, this time in a window-
  reset condition (`order.timestamp_ns - stats.window_start_ns >
  window_duration_ns`, never true on a backward jump), found while
  investigating why the live dashboard's `MlAnomalyDetector` alerts
  clustered in an implausibly narrow score band. Measured impact: 47x more
  scored windows crossing the live 0.7 threshold in a matched before/after
  comparison. Fixed with the same kind of reset-on-regression condition
  `AmbientTracker` got.
- **`AlertStore::list_recent_alerts()`'s ordering** (`db/`, documented in
  full in `cpp/db/alert_store.cpp`'s comments): `ORDER BY window_start_ns
  DESC` used the synthetic event clock as a stand-in for real recency,
  found while investigating why `WashTradeDetector` and
  `MarkingTheCloseDetector` had disappeared from the ALERTS tab's detector
  filter dropdown (itself populated only from the currently-loaded 250-row
  window — a real, if different, reason those two detectors could be
  legitimately absent, but not the one actually in play here). Both
  detectors were confirmed still firing via the alert_id-bounded
  live-sampling method established during the original stall investigation
  above; the dropdown's absence was instead traced to genuinely recent
  alerts sorting *behind* older ones because their synthetic timestamp
  happened to land in an already-passed band. `query_alerts_by_account()`
  and `query_alerts_by_detector()` had the identical pattern and were
  fixed alongside it; `dashboard/src/tabs/AlertsTab.tsx`'s own "sort by
  time" column comparator had it too (fixed to sort by `alert_id`,
  matching the backend). `query_alerts_by_time_range()` was checked and
  left unchanged — it uses `window_start_ns` as an explicit, caller-chosen
  filter range, not an implicit recency proxy, which is a legitimately
  different use of the same field.

Three independent modules (`detectors/`, `ml_client/`, `db/` — plus the
dashboard itself), three independent discoveries, one root cause each
time: code across this project assumed `fix::Order::timestamp_ns` (and
anything downstream of it, like `Alert::window_start_ns`) is monotonically
non-decreasing within a live process's lifetime, which has never been true
of `cpp/api/main.cpp`'s demo feed. The pattern is now well enough
understood that a future instance should be recognizable immediately: any
code that compares two timestamps with a one-directional inequality
(`new_ts - old_ts > threshold`) or sorts/orders by a `*_ns` field expecting
it to track real recency is a candidate, and the fix is either symmetric
distance (`std::llabs`) for windowed-retention logic or "prefer a real
monotonic proxy" (an auto-increment ID, a `std::chrono::steady_clock`
capture) for anything claiming to represent insertion or arrival order.
