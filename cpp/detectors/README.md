# cpp/detectors/ — detection layer design decisions

Companion to each detector's own header comments. This is the narrative
version: the interface deviation from the architecture doc, why each
detector's algorithm is shaped the way it is, and — for
`SpoofingLayeringDetector`, the phase's explicitly highest-scrutiny piece —
the exact scoring formula and how the test suite proves it.

## `IDetector::evaluate()` takes `DetectorEvent` (Order or Execution), not just `Order`

The architecture doc's original interface snippet declared
`evaluate(const OrderBook&, const Order&, const AccountRegistry&)`.
Implementing the five detectors this phase actually specifies surfaced a
real gap, not a style preference: `WashTradeDetector` and
`MarkingTheCloseDetector` are both fundamentally about *executions* — a
wash trade is a completed cross between related accounts, not an inference
from resting order state, and "concentrated activity near the close" means
executed volume, not orders merely resting in the book. Neither can be
implemented correctly against Order-only input. `DetectorEvent` (a local
`std::variant<Order, Execution>` — see `detector_event.hpp`, deliberately
not a reuse of `tse::ingestion::IngestionEvent`, to avoid pulling an
undocumented `ingestion/` dependency into `detectors/`) is the minimal fix.
`P2_trade_surveillance_engine_architecture.md` §3 has been updated to
match, with a note explaining why.

## `AccountRegistry`: a second, independent Account/Entity model

`cpp/simulator/types.hpp` already has an `Account` struct with exactly this
shape (`account_id`, `beneficial_owner_id`, `entity_type`,
`linked_account_ids`) and its own `AccountRegistry`
(`cpp/simulator/account_registry.hpp`) — but that class is built for
*random account assignment during synthetic generation* (`random_linked_pair()`,
`random_independent()`), a fundamentally different job from the fast
by-account-id lookup detectors need. `detectors/` depends only on
`orderbook/` per the architecture doc's dependency table, so
`tse::detectors::AccountRegistry` is a deliberate, independent
implementation of the same schema, not a shared type — the same pattern
already established between `fix::Order` (live-mode) and
`simulator::Order` (evaluation-mode, carries `ground_truth_label`).

`is_related(a, b)` is the single relation every detector actually consults:
identical accounts (self-trade), same beneficial owner, or an explicit
link (checked in both directions even if only declared one-way). An empty
`beneficial_owner_id` never matches another empty one — "unknown owner"
is a conservative default, not itself a relationship; manufacturing false
positives out of missing registry data would be worse than under-detecting.

## `Alert.score`: continuous, not boolean

Every `Alert` carries a `score` in `[0, 1]` by convention, not just a
fired/didn't-fire flag — this is what makes Phase 10's "threshold sweep +
confusion matrix per detector" possible later without re-running detection
logic per threshold. Each detector applies its own configurable threshold
internally (constructor parameter, sensible default) and only returns an
`Alert` once its own score clears that bar. `WashTradeDetector` and
`FrontRunningDetector` are deterministic rules (conjunctions of already-
binary conditions), so they always emit `1.0` when they fire — a
continuous score there would just be re-deriving the same yes/no facts
without adding information. `SpoofingLayeringDetector` and
`StatisticalBaselineDetector` are genuine heuristics with real internal
scoring math.

## `SpoofingLayeringDetector` — the highest-scrutiny detector this phase

Scores three signals named directly in the build guide, plus a fourth this
class's own name calls for:

1. **`depth_score`** — the order's share of visible depth *at its own
   price level*, captured the instant it's inserted
   (`order.qty / qty_at_price(side, price)` right after `apply()`, so the
   level already includes this order). Per-level, not per-side-total: the
   suspicious quantity is "how much of *this specific queue* would move if
   this order weren't there."

2. **`speed_score`** — linear from 1.0 (cancelled instantly) to 0.0
   (cancelled at or past `slow_time_in_book_ns`, default 5s). A fast
   cancel alone isn't damning — it's a real tell only combined with the
   other signals, since spoofing only works if the order never risks
   executing.

3. **`move_score`** — did the *opposite* side's best price move favorably
   (for a resting Buy: ask rose; for a resting Sell: bid fell) between
   placement and cancellation, by at least `min_opposite_price_move`?
   **Deliberately the opposite side, not the same side** — an earlier
   design compared the *same* side's best price before vs. after
   cancellation, which is self-referentially distorted: if the spoofed
   order was itself the best price on its side, cancelling it necessarily
   moves that side's own best price, conflating "the market moved during
   my life" with "I just left." Removing a resting order can never move
   the *opposite* side's best price, so this signal can't be distorted by
   the very cancel being evaluated. It also matches the actual accused
   mechanism more directly: a large resting order creates a false
   impression of supply/demand that shifts what the *other* side is
   willing to trade at.

4. **`layering_score`** — how many *other* orders the same account
   currently has resting on the same side of the same instrument,
   saturating at `layering_saturation_count` (default 3). This is what
   actually engages with "Layering" in the class name, beyond the three
   explicitly-named signals. **Deliberately additive**
   (`layering_bonus_weight * layering_score`, default weight 0.15 — not a
   fourth signal folded into the primary average): layering is a
   compounding aggravator of an already-suspicious single-order pattern,
   not independent evidence that should let a textbook non-spoof order
   fire purely because the account happens to have other legitimate
   resting orders elsewhere.

```
combined = clamp((depth_score + speed_score + move_score) / 3
                  + layering_bonus_weight * layering_score, 0, 1)
```
Fires iff `combined >= alert_threshold` (default 0.6).

**A deliberate, tested threshold property**: any two of the three primary
signals maxed out is already `2/3 ≈ 0.667`, above the default threshold —
so a dominant order cancelled almost instantly fires even with *no*
observed price move (`DominantOrderCancelledVeryFastFiresEvenWithoutObservedPriceMove`).
This is intentional, not an artifact of sloppy weighting: a large order
that vanishes in milliseconds is itself a recognized "flashing" pattern,
and requiring unanimous agreement across all three signals would make the
detector miss that. `ModerateSignalsAloneWithoutLayeringDoNotFire` /
`ModerateSignalsWithConcurrentLayeredOrdersFires` is the paired test that
specifically isolates the layering bonus's marginal contribution (0.583 →
0.733, crossing the 0.6 bar only with the bonus).

**State and lifecycle**: a per-`order_id` map tracks each resting order's
placement snapshot. A **Replace** retires the old `order_id`'s entry
without emitting anything and starts an entirely fresh lifecycle under the
new `order_id` — re-using the original placement snapshot across a
price/qty amendment would conflate two different resting postures.
`ReplaceRebasesLifecycleOntoNewOrderIdentity` deliberately sets up
different depth ratios and different time-in-book outcomes depending on
whether the old or new placement is used, so a rebasing bug would produce
a detectably different score, not just a plausible-looking one.

12 tests total, covering: the textbook full-signal case, two independent
single-weak-signal-among-strong-ones non-fires, the 2-of-3 threshold
property, the layering bonus's isolated marginal effect (paired
fire/no-fire), full-execution retiring a tracked order (never fires even
if a stray cancel follows), an untracked cancel being silently ignored,
Replace rebasing, layering-count account isolation (another account's
concurrent orders never count toward this account's layering score), and a
custom-threshold test proving the config is load-bearing for Phase 10's
planned sweep.

## `MarkingTheCloseDetector` — a floor caught while designing its own tests

No `Instrument`/session registry is available to `evaluate()` (only
`OrderBook`, `DetectorEvent`, `AccountRegistry`), so the per-instrument
session close schedule is supplied directly via
`close_time_ns_by_instrument` in the config.

While designing this detector's own test scenarios, a real gap surfaced:
a share-of-window-volume metric evaluated incrementally is trivially 100%
for whichever account(s) are party to the very first trade of an
otherwise-empty window — a percentage threshold alone can't distinguish
"genuinely dominates a busy close" from "happened to trade first, before
anyone else had a chance to." Fixed by adding
`min_total_window_qty_threshold` (default 500): no concentration check is
even attempted until the window's total volume clears that floor. This
wasn't a design decision made in the abstract — it was caught the same way
the order book's crossed-book question was caught in Phase 4: by working
through concrete hand-constructed scenarios before writing assertions.

Both sides of a matched trade are credited with that quantity toward their
own window volume (a counterparty is just as "active near the close" as
the initiator), while total window volume counts each `Execution` once —
so two counterparties who mostly trade with each other can each
legitimately show a high, even >50%, individual share. Alerts once per
`(instrument, account)` per detector lifetime, not once per qualifying
execution.

## `FrontRunningDetector` — a deterministic rule, not a heuristic

Fires when a large incoming New order (`qty >= min_large_qty_threshold`)
is preceded, within `lookback_window_ns` and on the same instrument and
side, by a smaller New order (`qty <= large_qty * max_leader_to_large_size_ratio`)
from a *related but different* account. The same-account case is
explicitly excluded — an account "front-running" its own order isn't a
meaningful concept, and without the explicit check `is_related(a, a)`
being trivially true would otherwise let it through
(`SameAccountSequencingAgainstItselfDoesNotFire` tests this directly, not
just the registry-level property already covered in
`account_registry_test.cpp`). A rolling per-`(instrument, side)` window of
recent New orders, pruned on every insertion, is what makes matching a
large order against something placed moments earlier cheap without
re-scanning full order history.

## `StatisticalBaselineDetector` — deliberately the simplest detector here

This exists specifically to be the naive comparison point Phase 10
measures the pattern-aware detectors against ("how much better than a
naive z-score is your spoofing detector"), so it should be genuinely
naive, not a disguised heuristic. Tracks a running mean/variance of New
order quantity per `(account, instrument)` via Welford's algorithm
(numerically stable for a long streaming accumulator, unlike naively
summing squares), and z-scores each new order against the stats *as they
stood before* that observation — scoring against pre-update stats is
deliberate, since folding a point into its own mean/variance first damps
its own deviation and makes genuine outliers systematically harder to
catch the more extreme they are. A `stddev == 0` guard (all-identical
baseline) prevents a division by zero from ever being reached, tested
directly with an extreme outlier that still doesn't fire
(`AllIdenticalBaselineNeverFiresEvenForExtremeOutlier`) — proving the
guard suppresses the check rather than, say, producing `inf`/`nan` and
firing on everything.

## Verification

58 detector-specific tests (11 `AccountRegistry`, 8 `WashTradeDetector`,
12 `SpoofingLayeringDetector`, 8 `MarkingTheCloseDetector`, 10
`FrontRunningDetector`, 9 `StatisticalBaselineDetector`), all passing on
the first run against hand-computed expected values — including several
`SpoofingLayeringDetector` and `StatisticalBaselineDetector` scores
checked to 1e-6 or exact-value precision, not just "fires vs. doesn't."
Every `SpoofingLayeringDetector`, `MarkingTheCloseDetector`, and
`WashTradeDetector` test applies its events to a **real** `OrderBook`
(Phase 4's already-verified implementation) before invoking the detector,
not a mocked book — `apply_and_evaluate()` in
`spoofing_layering_detector_test.cpp` mirrors the live pipeline's own
"orderbook applies the update, then detectors evaluate" ordering exactly.

Run three ways from fresh builds:
- **Benchmark (Release):** full project suite passes.
- **ASan:** clean.
- **TSan:** clean (single-threaded this phase, same as `orderbook/` —
  Phase 6 is where this gets wired into the concurrent live pipeline).
