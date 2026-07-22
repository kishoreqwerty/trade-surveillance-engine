# Exact stylized facts → exact code constants

This is the actionable checklist for Phase 11: every hardcoded constant in
`simulator/` that stands in for a real market-microstructure fact today,
what stylized fact replaces it, and where in the code it lives. Written so
that once real WRDS/TAQ numbers exist, recalibrating is "replace this
constant," not "figure out what to calibrate."

## What TAQ can and can't give you

Standard WRDS TAQ (Daily TAQ / Millisecond TAQ) is a **trade-and-quote
tape**: consolidated executed trades (`CT`) and NBBO quotes (`CQ`/NBBO).
It has **no raw order or cancel messages** — there's no order-level feed
(no ClOrdID, no New/Cancel/Replace) in standard TAQ, only what actually
printed to the tape or updated the NBBO. That constrains what can be
calibrated *directly* vs. by proxy:

| Stylized fact | Directly in TAQ? | How to get it |
|---|---|---|
| Trade size distribution | **Yes** | `CT` table's `SIZE` field |
| Bid-ask spread distribution | **Yes** | `CQ`/NBBO `BID`/`ASK` |
| Intraday price volatility | **Yes** | derived from `CT` prices or NBBO midpoint |
| Order arrival rate | **No** | proxy: trade + quote-update frequency (undercounts true order rate, since most orders never print or move the NBBO) |
| Cancellation rate | **No** | proxy: quote-to-trade ratio (QTR) — quote updates per trade, a standard market-microstructure proxy for order churn/cancellation. If your WRDS access also includes NASDAQ TotalView-ITCH (a separate product from TAQ, not always bundled), that has real order/cancel messages and would be strictly better — check for it, but don't assume it's there. |

Be upfront about this distinction in whatever you write up for Phase 11 —
"TAQ-derived" should mean what it actually is, proxy stats included, not
be a story that gets fuzzier under scrutiny.

## Known pitfalls found against real data

Four real problems showed up across this project's engagement with an
actual WRDS export (10 tickers, 4 days, 11:00-12:30 midday window). All
four are now fixed in the script. (A related but distinct issue —
basket-wide liquidity heterogeneity making a single spread table
non-representative for illiquid names — isn't a bug and isn't listed
here; see "Deferred: Instrument Liquidity Tiering" below.) Read all four
before trusting `stylized_facts.json`'s numbers.

**Zero-size rows (trade size and quote size) — fixed, root cause
confirmed.** The script originally applied no filtering at all:
`read_trades()` took whatever was in the `size` column, `read_quotes()`
didn't even read `bidsiz`/`asksiz`. Both are now dropped at the read
boundary, with dropped-row counts printed by `main()`.

Zero-size quotes (one-sided/withdrawn — price populated, no real size
resting there) are a normal, low-magnitude thing to filter. Zero-size
*trades* went through two wrong hypotheses before landing on the real
one, worth recording so the reasoning is traceable:

1. First guess: "correction/cancellation records for a busted print" — a
   real but *rare* TAQ phenomenon. Against a real WRDS export the filter
   dropped **21.6%** of trade rows, a rate that hypothesis couldn't
   plausibly explain.
2. Checked via `tr_corr`/`tr_scond` (Trade Correction Indicator / Sale
   Condition), found by re-pulling `trades.csv` with those two columns
   added (found via `information_schema`, same way `sym_root`/`bidsiz`
   were): **100% of size<=0 rows carry `tr_corr=='00'`** (uncorrected/
   normal) — this *ruled out* the correction/cancellation theory
   entirely, it was never that.
3. **Confirmed real cause**: 95.5% of the size<=0 rows carry sale
   condition `'I'` (odd lot indicator) — a known TAQ reporting quirk
   where certain odd-lot prints report `size=0` in the SIZE field instead
   of the actual (sub-round-lot) share count. This also explains why the
   rate was higher in AAPL/MSFT specifically: odd-lot trades cluster in
   the most heavily-traded mega-caps.

The filter itself (`size <= 0` excluded from the trade-size distribution)
needed **no code change** once this was known — a size=0 row was never
usable trade-size data regardless of which of the three explanations
turned out to be true. `compute_stylized_facts.py --diagnose-trades` and
`--diagnose-corrections` (built during this investigation) remain
available for any future anomaly of this shape.

On `tr_corr='00'` meaning "uncorrected/normal": real WRDS-hosted SAS
example code (the `taq6.sas` macro at wrds-www.wharton.upenn.edu) filters
trades with `tr_corr = '00'` to get clean prints — good practical
evidence for that specific value, and now directly confirmed by the
crosstab above (100% of the known-odd-lot rows carry it). This project
could **not** retrieve the complete NYSE Daily TAQ Client Specification
code table from either PDF version checked (v2.2a, v3.0) — both failed
to extract usable text, the same failure mode as the CFTC Sarao complaint
PDF (see `cpp/simulator/abuse/sarao_case.hpp`'s own citation-tier notes)
— so the *other* `tr_corr`/`tr_scond` values beyond `'00'`/`'I'` are
still not verified against a primary source, only whatever the diagnostic
tools reported empirically.

**Wide spreads (raw per-venue quotes vs. true NBBO) — resolved.**
`compute_spread_stats()` reports a scale-invariant `relative_bps` view
(ticks alone can't say whether 185 ticks is wide — that's ~1% for a $195
stock, unremarkable for a $18,500 one) and an `implausible_fraction`
(share of quotes over 200bps relative spread, flagged for investigation,
never silently dropped). Root cause was confirmed to be exactly the
mechanism this section originally warned about: the first `quotes.csv`
pull was against `taqmsec.cqm_2026`, raw per-exchange consolidated
quotes, not NBBO — a single venue's own two-sided quote, especially a
thin/backup exchange's deliberately wide, non-competitive "defensive"
quote (a well-documented real phenomenon), can be genuinely
zero-size-filtered-clean and still be far wider than the true national
best bid/offer. Re-pulled against the actual NBBO table
(`taqmsec.nbbom_YYYYMMDD`, `best_bid`/`best_ask`/`best_bidsiz`/
`best_asksiz` instead of `cqm`'s raw `bid`/`ask`/`bidsiz`/`asksiz`) —
`implausible_fraction` came back `0.0` with realistic per-symbol medians
(AAPL 3 ticks, MSFT ~5 ticks). This script had no venue/exchange column
to detect or correct the original problem itself; it could only be
resolved at the query stage, which is what happened.

**Extreme price-jump outliers in `intraday_price_volatility` — fixed,
root cause confirmed.** The aggregate mean (55.4 ticks) came out far
above the aggregate's own p99 (32.2 ticks) — only possible with a small,
heavy tail beyond p99. `by_symbol` breakdown (added to
`compute_volatility_stats()` specifically for this) showed it was **not**
evenly distributed — JPM was the dominant outlier (p99=27,423 ticks,
max=28,948 ticks, a $289.48 jump) with RF a smaller secondary one, every
other symbol in the normal 18-90 tick range. `find_price_jumps()`
(`--find-price-jumps`) pinpointed the exact row pair: a repeated,
clean alternation between ~$17.84 and ~$307 throughout the day.

**Root cause was not a bad print** — it looked like one from the
aggregate stat alone, but the real cause was a query-scoping bug: the
trades/quotes queries filtered `sym_root IN (...)` without also
restricting `sym_suffix`, so JPM common stock (~$300s) and JPM's
preferred share series (JPM-C, JPM-D, JPM-J, ..., trading independently
around $17-25) were pulled as one interleaved series under a single
`sym_root`. RF has preferred series too, explaining its smaller
secondary anomaly — and once `sym_suffix` was restricted to common stock
only (verified against `information_schema` and each suffix's own price
range, not assumed), RF's *spread* numbers normalized as a side effect
as well, confirming it was the same root cause, not a coincidence.

The 10% relative-price-change filter (`IMPLAUSIBLE_PRICE_CHANGE_FRACTION`
in `compute_volatility_stats()`) was built and applied *before* the real
cause was known, as a symbol-agnostic defensive bound — grounded in SEC's
Limit Up-Limit Down mechanism (NMS Tier 1 stocks above $3 get a trading
pause past a 5% band from a reference price, so a genuine single
print-to-print jump beyond that essentially cannot happen in ordinary
trading). It turned out not to be the fix for this particular problem —
the `sym_suffix` correction was — but it's kept in deliberately, not
removed now that the real cause is known: the LULD justification was
never tied to this bug specifically, and a real bad print (decimal error,
corrupted field, timestamp mispairing — all independently documented TAQ
phenomena) is still a risk on any future pull. Confirmed dormant on the
corrected data: `implausible_pairs_dropped: 0` across all ten symbols
after the `sym_suffix` fix, exactly what a properly-scoped defensive
bound should show once the actual problem it might have caught isn't
present. `--selftest` separately proves it doesn't over-trigger (a
legitimate 8% single-print move survives, a JPM-shaped ~130% one doesn't)
independent of whatever real data does or doesn't need it on a given day.

**Order-arrival-rate divisor (fixed-session-length assumption) — fixed,
root cause confirmed.** `compute_all()` originally divided
`order_arrival_rate_proxy_per_second`'s numerator (trades + quote updates
per symbol-day) by a hardcoded `6.5 * 3600` ("standard US equity regular
session"). That's wrong for data that was never pulled for a full
session — this project's export is scoped to an 11:00-12:30 midday
window (~90 minutes), confirmed directly against `trades.csv`'s own
`time_m` values (span 11:00:00 to ~12:29:5x across every symbol-day
checked). Dividing a 90-minute numerator by a 390-minute denominator is a
straightforward units mismatch: it understated the arrival rate by
exactly `23400/5400 = 4.33x`. Verified precisely by re-running
`read_trades()`/`read_quotes()`'s own filtered output through both
divisors: as-shipped **3.85** events/sec/symbol vs. corrected **16.69**
events/sec/symbol.

Fixed by `_compute_window_seconds()`, which derives the window from the
data's own `max(time) - min(time)` per symbol-day (averaged across all
40 symbol-days), combining trades *and* quotes (either alone risks
clipping the window's true edges) — not a second hardcoded constant, so a
future pull with a different window length stays correct automatically.
`order_arrival_rate_proxy_per_second` is now a small object
(`value`/`window_seconds_used`/`description`) rather than a bare number,
so the window actually used is always visible next to the rate itself.
Real corrected window: 5398.5s (≈89.98 min), matching the empirically
observed pull window almost exactly. `_run_arrival_rate_window_selftest()`
proves the derivation on two synthetic symbol-days with known, different
spans (60s/120s), proves a quote genuinely widens the bound beyond what
trades alone would show, and proves the old bug's divisor is nowhere
near the corrected rate (>100x off) — not just that the function runs.

`simulator/`'s instruments (`ACME`, `GLBX`, `NDEX`, ... —
`cpp/simulator/instrument_universe.cpp`) are **fictional tickers**. TAQ
only covers real US-listed equities/ETFs (no FX, no fixed income — the
other two asset classes this simulator also generates have no TAQ
equivalent at all). So this calibration is necessarily: pull real stats
from a basket of **real, liquid, common stocks**, extract distributional
parameters, and apply those distributions generically to every synthetic
instrument regardless of its fictional ticker or asset class — an
approximation, and worth saying so explicitly rather than implying
per-symbol precision that isn't there.

Pick a basket that spans a liquidity range rather than one name — a
single stock's stylized facts are a data point, not a stylized fact.
Suggested shape (adjust to what's actually easy to pull):
- 3-4 mega-cap, high-liquidity names (e.g. from the Dow or S&P 100)
- 3-4 mid-cap S&P 400-tier names, lower average daily volume
- Avoid: ETFs (different microstructure — arbitrage-driven, not
  representative of single-name order flow), anything under ~$5 (penny-
  stock microstructure is its own regime), anything that had a
  stock split/special dividend/M&A event in the date range.

## Date range

A short window (5-10 trading days) from an **ordinary, non-event**
period — avoid FOMC days, earnings dates for your chosen names, opex/quad-
witching Fridays, and obviously avoid crisis days (the whole point is a
"typical" baseline; the one deliberate crisis-day exception is the
separate, real-case Sarao/Flash-Crash validation below, which doesn't use
TAQ at all). Recency isn't critical — a representative week from any
recent, uneventful period is fine; just confirm WRDS has finished
processing that range before you pick it (there's usually a short lag).

## Export format

**Corrected against a real WRDS export** (this section originally
documented a guessed schema, written before anyone had actually queried
WRDS — it was wrong: WRDS Daily TAQ's real CSV column names are
`sym_root`/`time_m`/`bidsiz`/`asksiz`, not `symbol`/`time`/`bidsize`/
`asksize`. `compute_stylized_facts.py`'s `read_trades()`/`read_quotes()`
now read the real names below; if a future WRDS TAQ product uses
different names again, `--selftest`'s CSV round-trip check will catch it
immediately via a `KeyError`, the same way it caught this one — see
`_run_csv_roundtrip_selftest()`'s own comment for why the original schema
shipped unverified in the first place.)

Two flat files, WRDS's own column names, unchanged from what the query
returns — no reshaping needed:

**`trades.csv`** (from TAQ's `CT`/Daily TAQ trades table):
```
sym_root,date,time_m,price,size
AAPL,2026-06-03,11:00:01.123456,182.45,100
```
`time_m` is WRDS's actual field name (not `time`), typically
`HH:MM:SS.ffffff` (microseconds) but the script also tolerates more
fractional digits (nanosecond-precision TAQ products truncate to
microsecond) or none at all. Regular-session trades only (exclude pre/
post-market, exclude trades with a non-empty `COND`/condition code that
marks it as a non-standard print — TAQ's trade condition codes flag
things like odd-lots, average-price trades, etc. that would skew the size
distribution).

**`quotes.csv`** (from TAQ's `CQ`/NBBO table — use the pre-computed NBBO
table if your WRDS product has one, not raw per-exchange quotes; you want
the *national* best bid/offer, not one venue's):
```
sym_root,date,time_m,bid,ask,bidsiz,asksiz
AAPL,2026-06-03,11:00:01.050000,182.44,182.46,500,300
```
`bidsiz`/`asksiz` (not `bidsize`/`asksize`) — WRDS's real column names.
`compute_stylized_facts.py` doesn't currently read these two columns
(only `bid`/`ask`), so their exact values don't matter for this script,
but they're part of what a normal NBBO query returns and don't need to be
dropped before export.

**Easiest extraction path**: WRDS has a Python client (`pip install wrds`)
that queries the same Postgres-backed database the web query UI uses —
write a script that runs the SQL and writes these two CSVs directly,
rather than round-tripping through the web UI's manual CSV download. The
exact schema/table names (`taqm_2024.ctm_20260303`-style, or similar)
depend on which TAQ product your WRDS subscription includes and changes
periodically — the query UI will show you the real current table/column
names once you have access; don't rely on names cited from outside
without checking against what you actually see.

## Constants to replace, file by file

### `cpp/simulator/baseline_generator.cpp`

| Constant | Current value | Stylized fact to replace it | Rough calc |
|---|---|---|---|
| Order qty | `uniform_int64(rng, 1, 10) * 100` (flat 100-1000, uniform) | Trade size distribution shape | Fit `SIZE` percentiles from `trades.csv`; likely replace flat-uniform with something that has real fat tails (e.g. sample from empirical percentile buckets, or a lognormal fit) |
| Price walk step | `mid + tick_size * uniform(-2.0, 2.0)` per order | Intraday volatility (price change per unit time/per trade) | Realized volatility from `trades.csv`/`quotes.csv` midpoint, converted to "average tick move per order" at this generator's order rate |
| Price offset from mid | `offset_ticks = sample_from_percentiles(rng, kSpreadOffsetPercentiles)` | Bid-ask spread distribution, **liquid tier only** (AAPL/MSFT/XOM/RF-representative — see "Deferred: Instrument Liquidity Tiering" below for the DE/HAS/TGT gap) | Half of `spread.aggregate` percentiles (ticks), floored — see code comment for why halving |
| Fill/partial/cancel/open split | fixed `0.60 / 0.15 / 0.15 / 0.10` — **left unchanged**, sanity-checked against QTR, not substituted (see below) | Cancellation-rate proxy (QTR) | No exact map (TAQ has no true cancel rate — see caveat above); use QTR's relative *level* to judge whether 15% cancelled is in the right ballpark, not as an exact replacement |
| Cancel dwell time | `uniform_int64(rng, 500ms, 30s)` | — (no direct TAQ equivalent; same QTR-timing-proxy caveat) | Best effort only |
| `orders_per_second` (`SimulatorConfig::baseline_orders_per_second`) | struct default `5.0` (dead — every real call site already overrode it with its own ad hoc value); `harness/main.cpp`'s 3 call sites now `kEmpiricalOrderArrivalRatePerInstrumentPerSecond × that config's own instrument count`; `simulator/main.cpp`'s CLI demo (`2.0`) deliberately left untouched (own comment: 5min session, "small enough to eyeball" — scaling it up would flood the demo) | Order arrival rate proxy, EQUITY ONLY | `kEmpiricalOrderArrivalRatePerInstrumentPerSecond = 16.69` (`simulator.hpp`) — see below for why it's multiplied by *total* instrument count, not just equity count |

**Row 6 — confirmed: multiplying by total instrument count (not just
equity count) matches existing behavior, not a new mismatch.** Checked
directly against `random_utils.hpp`'s `pick_random()`
(`std::uniform_int_distribution<size_t> dist(0, values.size() - 1)`) and
`baseline_generator.cpp`'s event loop: the generator runs one Poisson
process at a single flat rate (`baseline_orders_per_second`) over the
*whole* instrument universe, then picks the instrument for each event
uniformly at random from **all** instruments — equity, FX, and fixed
income alike, no asset-class weighting anywhere in the code. So the
realized per-instrument rate is always
`baseline_orders_per_second / total_instrument_count`, for every
instrument regardless of type — that dilution already existed before
this row, at the old guessed values (`2.0`/`3.0`) just as much as at the
new calibrated one. Multiplying the equity-calibrated rate by the total
instrument count reproduces that same dilution exactly, so FX/fixed
income end up generating orders at the equity-calibrated per-instrument
rate — not because that's realistic for FX/fixed income (it isn't, and
there's no TAQ equivalent to check it against either way), but because
it's what today's uniform-selection mechanism already does to every
instrument, just scaled from a guess to a measured number.
Asset-class-differentiated rates remain out of scope — same position as
FX/fixed-income staying uncalibrated elsewhere in this document.

**Row 6 follow-on — the ~83x total order-volume increase exposed three
real problems downstream, investigated one at a time before any fix, per
this project's standing rule: don't touch thresholds until the mechanism
is understood.** Running the harness end-to-end with the calibrated rate
collapsed precision for `WashTradeDetector`/`SpoofingLayeringDetector`
and recall for `MarkingTheCloseDetector`. Root-caused via a rate sweep (3
to 250.35/sec) plus direct book-state/detector-state tracing, not
assumed:

1. **`WashTradeDetector` — a real, confirmed simulator bug, fixed.**
   `baseline_generator.cpp`'s counterparty selection only ever re-drew to
   avoid an *exact* self-match, never checked relatedness (same
   beneficial owner or explicit link). Predicted FP from account-pool
   geometry alone (`accounts_related()` collision probability × execution
   count) matched observed FP almost exactly at both the old and new
   rate (2.14 predicted vs. 2 observed; 154.0 predicted vs. 154 observed)
   — conclusive, not circumstantial. Fixed by adding
   `accounts_related()` (mirroring `cpp/detectors/account_registry.cpp`'s
   `is_related()`) to the retry loop. Result: P=1.000, R=1.000, F1=1.000
   at every rate tested, 3 to 250.35/sec — actually better than the old
   rate's own P=0.938, since the bug was present there too, just less
   visible.

2. **`MarkingTheCloseDetector` — a real generator gap, first patched
   around, then genuinely fixed in Phase 11.5 (below).** See
   `cpp/harness/README.md`'s "Phase 11 update" under this detector's own
   findings section for the original writeup: the scenario's own volume
   was a flat constant that didn't scale with ambient closing-window
   volume, diluting its concentration share toward zero as order density
   rose. Fixed on the generator side (`abuse/marking_the_close.cpp`) by
   anchoring scenario volume to *expected* ambient volume, computed from
   `orders_per_second`/instrument count. That alone surfaced (not fixed)
   a real structural limitation — `concentration_threshold` being
   unreachable with 2+ accounts sharing the scenario's volume — which
   Phase 11.5 then closed properly; see that section below for the full
   story, including a real detector-aggregation fix and two false alarms
   in this investigation's own diagnostic tooling that turned out not to
   be product bugs.

3. **A measurement confound, not a product bug, found and fixed:**
   `generate_simulation()` shared one `std::mt19937_64` between baseline
   flow and abuse-scenario injection. Since baseline flow's draw count
   varies with `baseline_orders_per_second`, changing the order rate
   silently shifted every abuse scenario's own random parameters too
   (which instrument, timing jitter, price-step direction) — confounding
   any attempt to isolate a rate-driven effect on detector recall from
   pure RNG-state drift. `SpoofingLayeringDetector`'s TP trajectory across
   the rate sweep was wildly non-monotonic before this fix (36, 25, 15,
   8, 16, 8, 20, 12, 4, 0, 4) and became overwhelmingly monotonic after
   (33, 32, 31, 24, 18, 13, 13, 6, 9, 4, 4 — one small blip, not the
   previous zigzag). `FrontRunningDetector`'s TP went from fluctuating
   18-30 across the same sweep to essentially flat at 24 (25 at the top
   rate) — the same mechanism, independently confirmed on a detector
   whose own logic has nothing to do with density. Fixed by giving
   abuse-scenario generation its own `abuse_rng` stream
   (`config.random_seed ^ 0x9E3779B97F4A7C15ULL`, the standard
   golden-ratio hash-mixing constant, used only to decorrelate the two
   seeds), independent of how many draws baseline flow consumes.

With the confound removed, `SpoofingLayeringDetector`'s remaining
precision collapse (FP climbing 9→14→19→...→854 across the sweep, no
cliff) is now a clean, isolated density effect — not entangled with
RNG-ordering noise. Not yet investigated further: whether that remaining
density-sensitivity (`move_score`/`layering_score`, both structurally
more likely to trip by chance at higher ambient volume — see the
diagnosis two turns back) needs a fix or is itself an honest, documented
limitation, same category of decision as `concentration_threshold` above.

## Phase 11.5 — closing MarkingTheCloseDetector's evasion gap

**A deliberate, named follow-on, not a silent reopening of Phase 11's own
scope boundary.** Phase 11's own precedent (see Task 2's scope discussion
above) is that detector thresholds stay fixed; Phase 11.5 is an explicit
exception, scoped narrowly to the one structural gap found while fixing
`MarkingTheCloseDetector`'s generator-side volume anchoring: splitting a
scheme's volume across 2+ related accounts evaded a *per-account*
concentration check entirely, at any volume — a real, closeable gap, not
a calibration choice.

**1. Detector-side: beneficial-owner/linked-account aggregation.**
`MarkingTheCloseDetector::check_account()` now aggregates by group before
computing concentration share, reusing `AccountRegistry::is_related()` —
the exact relation `WashTradeDetector` already uses — via an incremental
union-find (`register_and_group()`/`find_group()`/`group_members()` in
`marking_the_close_detector.cpp`). Raw per-account storage
(`account_window_qty_`, `trade_ids_by_key_`) stays keyed by real
account_id; group aggregates are computed by summing over current members
fresh at check time, not by re-keying storage under a union-find
representative — representatives can change identity across a later
merge, so anything stored under one would silently go stale. Two new
tests prove this, including the specific case reviewed before
implementation: `RelatedAccountsSplittingVolumeAreAggregatedAndFireTogether`
(two related accounts, each individually below threshold, each trading
with its own *unrelated* counterparty — never with each other — combine
to clear it) and `GroupAlertedStatusSurvivesLaterCompositionChange` (a
third related account, discovered only when it first trades, joins an
already-fired group without spuriously re-firing it — sized so the
merged group's share would clearly clear 0.4 again if the alerted status
hadn't carried over, proving the suppression does real work).

**2. Generator-side: the scenario's own multi-account pool must actually
be related, or Task 1 has nothing to exercise.** `simulator.cpp`'s
marking_the_close loop drew `accounts_used` accounts via
`random_independent()` — guaranteed *unrelated* by construction (unique
`beneficial_owner_id` per independent account). Confirmed empirically: a
full rate sweep after Task 1 alone showed *zero* change in MTC's numbers,
because the evasion path Task 1 closes was never being exercised by this
project's own synthetic data. Fixed by drawing a genuine
`account_registry.random_linked_pair()` instead. This also required
capping `accounts_used` at 2 (was up to 3): the simulator's
`AccountRegistry` only models relatedness as fixed pairs, not arbitrary
groups, and extending it to 3-way linked groups was judged a bigger
data-model change than this fix called for — severity now spans 1-2
related accounts, not 1-3, an honest reflection of what's actually
supported.

**3. The qty-sizing formula needed revisiting twice, not once.** With
Task 1 aggregating the whole group, the old per-account ceiling
(`target_share = k/accounts_used`, needed because each account's own
share was capped at `1/accounts_used` of the total under the *old*
per-account check) no longer applied — continuing to divide was now
needlessly conservative, and would have left `accounts_used=2` scenarios
still under-clearing 0.4. Simplified to `target_share = k` directly
(algebraically a no-op for `accounts_used=1`, confirmed before
implementing, not just asserted). That exposed a second problem: `k`'s
existing range (`lerp(0.3, 0.85, severity)`) had been chosen as a
per-account target inside the old divided formula, where the real
achieved share never actually reached its nominal upper bound. Used
directly, 0.85 meant the scheme representing 85% of *all* window volume
at the "obvious" end of severity — implausible near-total market
domination. No verified real-enforcement-case citation exists for an
exact figure (checked, not assumed); reasoning from market-structure
logic instead (thin closing windows make real manipulation shares
plausible in the 40-65% range, not 85%+), revised to
`k = lerp(0.15, 0.65, severity)`. At severity=0.5, k=0.4 lands almost
exactly on the detector's own threshold — an expected consequence of a
linear severity dial crossing a fixed (not continuous-score) bar
somewhere, not something engineered around.

**4. Two false alarms in this investigation's own diagnostic tooling,
run down and ruled out before touching any more product code — the same
rigor as everywhere else in this project.** After all three fixes above,
a rate sweep still showed TP=0 for every rate. Root-caused via a
purpose-built standalone tool that drives the real `MarkingTheCloseDetector`
class against the real chronological execution stream (not a
reimplementation) — twice:
   - First pass: the diagnostic's own `AccountRegistry` was never
     populated (`accounts.add(...)` never called), so `is_related()`
     returned false for everything and grouping silently never
     triggered. A bug in the scratch diagnostic, not
     `marking_the_close_detector.cpp` — confirmed by fixing the
     diagnostic's account population and re-tracing: the real detector
     groups `[ACC-000201 ACC-000202]` correctly and fires at exactly the
     execution predicted by hand (share=0.4049 at the moment computed by
     hand, before any code was re-examined).
   - Second pass: with grouping confirmed working and legitimate group
     alerts firing (verified: the *same* alerts appear via the real
     Kafka-replay path used by the actual harness, not just the
     standalone tool), the harness's own confusion-matrix table still
     showed TP=0. Every one of the five legitimate group alerts scored
     0.40–0.42 — comfortably clearing the detector's own 0.4
     `concentration_threshold`, but below the evaluation harness's
     uniform cross-detector 0.5 threshold used for the headline table.
     This is the *exact* pre-existing "scoring-scale caveat" already on
     record in `cpp/harness/README.md` from Phase 10 (this detector's
     score literally *is* the concentration share, so a scenario that
     just clears its own bar scores 0.40–0.49) — not a new bug,
     rediscovered mid-investigation after losing track of it during a
     long diagnostic chain. Confirmed at the correct threshold: TP=54,
     FP=337, FN=51 at threshold=0.4 (main eval config, final calibrated
     rate) — recall 0.514, up from Phase 10's original 0.314 at the same
     threshold.

**Final numbers, full rate sweep (main eval config, threshold=0.4 — this
detector's own scale, per the caveat above), recall/precision:**

| rate/sec | 3 | 5 | 7 | 9 | 12 | 16 | 20 | 25 | 75 | 150 | 250.35 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| recall | 0.190 | 0.410 | 0.438 | 0.467 | 0.457 | 0.514 | 0.514 | 0.324 | 0.505 | 0.533 | 0.514 |
| precision | 0.606 | 0.729 | 0.687 | 0.653 | 0.632 | 0.587 | 0.607 | 0.694 | 0.340 | 0.199 | 0.138 |

Recall is genuinely usable across the whole range now (was uniformly 0 at
the headline threshold before this fix). Precision degrades smoothly with
density (FP climbing 13→337 across the sweep, no cliff) — the same
already-documented `min_total_window_qty_threshold=500` limitation
(still not fixed, still out of Phase 11.5's narrower scope, which only
covered the account-aggregation gap), now more visible because
2-account combinations have more ways to spuriously coincide early in a
still-thin window than single accounts did.

**Severity gradient at the final calibrated rate (threshold=0.4):**
recall 0.100 (severity 0.1) → 0.333 (0.3) → 0.514 (0.5) → 0.367 (0.7) →
0.207 (0.9) — non-monotonic, peaking at severity=0.5 rather than rising
smoothly to severity=0.9 as the other detectors' gradients do.

**Investigated and confirmed with a direct trace, not left as a plausible
guess** — this mattered enough to check properly: "more obvious abuse
caught less reliably" would be a real problem if detection itself were
failing at high severity. A purpose-built diagnostic drove the real
`MarkingTheCloseDetector` class and tracked, per scenario (all 15, at
severity 0.5/0.7/0.9), how many of its own trade_ids had executed by the
moment its alert fired. Result: **every scenario fired at every severity
tested — zero non-detections.** The decline is entirely in *what fraction
of a scenario's own trades get evaluation credit* before its one-time
alert fires and `alerted_` blocks further credit for that group:

| severity | avg. own-trade fraction credited before firing |
|---|---|
| 0.5 | 0.488 |
| 0.7 | 0.365 |
| 0.9 | 0.205 |

**Confirmed mechanism:** higher severity → higher target concentration
share (`k = lerp(0.15, 0.65, severity)`) → the group's incrementally-
accumulating share crosses the fixed 0.4 `concentration_threshold`
*earlier* in its own trade sequence (a bigger intended overshoot is
reached sooner, not later) → this detector's explicit, correct
"alert once per group lifetime" design (a real streaming detector
structurally cannot credit trades it hasn't seen yet at fire time)
permanently forfeits credit for whatever trades follow. Not a bug — a
property of the per-trade-ID recall metric this evaluation harness uses,
not of detection reliability. A scenario-level "did it fire at all"
metric would read 100% at every severity tested. Left as a per-trade
metric (not changed) since that's an evaluation-harness design choice
outside this investigation's scope, not a defect to fix.

**Row 4 (fill/partial/cancel/open split) — why it stayed a plausibility check, not a
substitution.** `quote_to_trade_ratio_cancellation_proxy` in `stylized_facts.json`
(n=40 symbol-days): p10=0.60, p25=0.66, p50=0.83, p75=1.78, p90=3.83, p99=6.57,
mean=1.46 quote updates per trade. The current split's cancel-to-fill ratio is
15/60=0.25 — same order of magnitude as QTR's low end (p10-p25 ≈0.60-0.66), well
below its mean (1.46) and upper range. That's "not obviously wrong," not
confirmation of correctness, and the split was left unchanged on that basis.

Explicitly **not** pushing the cancel rate up to close the gap toward QTR's mean,
for a reason beyond just "the mapping is imprecise": QTR conflates cancel-driven
and new-order-driven quote moves (any event that moves the NBBO counts, TAQ can't
tell which caused it), and the generator tracks every order individually, not just
the ones that happen to move the touch. A higher QTR at a given symbol doesn't
specifically indicate a higher cancellation rate — it could equally reflect more
new-order arrivals at the best price, more price volatility moving the touch, or
thinner top-of-book depth making the NBBO more sensitive to any single order. QTR
being higher than the current ratio is therefore not *directionally* informative
for this parameter, not merely too coarse to pin an exact number — there's no
principled adjustment to make from this signal, in either direction.

### `cpp/simulator/abuse/spoofing_layering.cpp` — the one Phase 10 flagged as actually broken, now fixed

| Constant | Old value | Fixed value |
|---|---|---|
| `dwell_ns` formula | `lerp(60s, 0.5s, severity) + jitter(0, 2s)` | `lerp(1.8x, 0.2x, severity) + jitter(±0.5s)`, `x` = the detector's own anchor (below) |
| `SpoofingLayeringConfig::slow_time_in_book_ns` (the *detector's* config, `cpp/detectors/spoofing_layering_detector.hpp`) | `5s` default | unchanged — kept as the fixed **anchor** the generator now derives its bounds from |

TAQ cannot give a real cancel-to-placement timing distribution (no
order-level cancel messages), so this was never a plug-in-real-numbers
fix — it's a self-consistency one. The bug: the old bounds only dropped
`dwell_ns` below the detector's fixed 5s threshold above severity ≈0.92,
so `speed_score` was exactly 0 — no signal at all — across the bottom
~92% of the severity range. Confirmed directly against a real replay:
all 12 layers in a severity=0.9 scenario scored `speed=0.0`. Fixed by
anchoring the generator's bounds to the detector's own 5s default
(`kSpeedScoreAnchorNs` in `spoofing_layering.cpp`) instead of two
independently-guessed constants: severity=0 dwells at 1.8x the anchor
(clearly slow), severity=1 at 0.2x (clearly fast), crossing the anchor
itself at severity=0.5 — `speed_score` now responds across the whole
range. Verified: the same real-replay trace that showed `speed=0.0`
twelve times now shows `speed` in the 0.5–0.7 range at severity=0.9.

**A second, deeper bug surfaced while fixing the first one and re-running
`ReplayRunnerKafkaTest` against the recalibrated baseline (Phase 11's own
`baseline_generator.cpp` changes)**: `SpoofingLayeringDetector`'s
`move_score` (did the opposite side's best price move favorably) had
*never* been reliably driven by this scenario's own construction — it
depended entirely on ambient baseline order flow happening to move the
right way at the right time. The old, less realistic baseline price walk
happened to cooperate often enough that this went unnoticed; the
recalibrated one (real percentiles, `p50=0` — the book stays flat much
more often — see Row "Price walk step" below) exposed it. Same
test-fragility class already caught elsewhere in this project (Phase 6's
unpaced producer test, Phase 1/5's own true-positive-case isolation
principle): a scenario asserting a known pattern fires should not depend
on unrelated randomness.

Fixed with a deterministic "anchor" mechanism in
`generate_spoofing_layering_scenario()`: a dominant reference order is
placed on the genuine side, aggressively priced near the edge of a
deliberately widened safe zone between reference and where the layers
start (`kAnchorSafeZoneTicks = 8`, up from the original 1-tick spacing —
also more realistic on its own terms, matching the Sarao case's cited
"three or four price levels from the best asking price"), then withdrawn
and replaced with a much less aggressive order partway through the dwell
window, before any layer cancels. This genuinely moves
`best_price(side_genuine)` in the direction `move_score` checks for,
regardless of ambient state — it does not rely on out-competing baseline
noise for "dominance" from a fixed offset the way an earlier version of
this fix incorrectly did (that version placed the "dominant" leg *between*
reference and the layers rather than *toward* the layers, which is *less*
aggressive than real ambient liquidity can be, not more — caught by
tracing the actual book state, not just re-running the one seed the test
uses). The anchor orders are deliberately left unlabeled (`kBaseline`
sentinel, not the scenario's own `ground_truth_label`) since they're
market-structure scaffolding, not the spoofing pattern itself — tagging
them would silently deflate Phase 10's measured recall (they can never
appear in `SpoofingLayeringDetector`'s `alert.order_ids`, since it only
ever names the layer being cancelled).

**Verified empirically across 50 different seeds** (not just the one the
regression test happens to use), per the standard this fix was held to:
fire rate went from 49.4% (pre-fix, with the ground-truth-label bug
present) to 81.0% post-fix, and — the bar that actually matters, since
it's what `ReplayRunnerKafkaTest` itself asserts — **every one of the 50
seeds** had the large majority of layers fire (10-12 of 14), comfortably
clearing "at least one alert," not just the specific seed baked into the
committed test. The remaining ~19% of layers that don't fire are
explainable, not random: the last layer to cancel in a batch has zero
concurrent-layering bonus, and two `SpoofingLayering` scenarios can land
on the same instrument in one simulation, occasionally overlapping in
timing.

## Phase 11.5, part 2 — density-normalizing SpoofingLayeringDetector

**Scope:** the second Phase 11.5 item — `move_score`/`layering_score` were
found (Row 6 follow-on, above) to be structurally density-sensitive: both
compared against fixed absolute constants (`min_opposite_price_move`,
`layering_saturation_count`) with no notion of what's normal for current
conditions, so higher ambient order-flow made either signal more likely
to cross its bar by chance alone, independent of genuine spoofing.

**Design:** a rolling ambient tracker per (instrument, side), pooled
across accounts, used to normalize both signals against *recent*
conditions instead of fixed constants:

- `move_score = moved_favorably ? clamp(1 - expected_moves_during_dwell, 0, 1) : 0`,
  where `expected_moves_during_dwell = recent_move_rate × effective_dwell_s`
  — a move that ambient churn alone would statistically produce during a
  dwell this long isn't evidence of this specific order's influence.
- `layering_score = clamp((concurrent - typical_concurrent) / layering_saturation_count, 0, 1)`
  — subtracts off "what's typical for anyone right now" before comparing
  against the saturation scale.
- `density_window_ns = 30s = 6 × slow_time_in_book_ns` (5s) — not an
  independent constant. Grounded in the detector's own existing dwell-
  scale anchor rather than a simulator-side calibrated rate, deliberately:
  detectors/ must not depend on simulator/, and this detector also has to
  work against real/replayed data (the Sarao case) where no synthetic
  calibration applies. 6x gives the window enough dwell-periods' worth of
  potential events that the rate estimate isn't dominated by single-
  observation noise, while staying short enough to reflect genuinely
  recent conditions, not session-wide history.

**Prediction stated before implementing** (per this project's standing
practice of predicting before verifying, not just checking after the
fact): `move_score` expected to stay roughly unchanged for the Sarao case
specifically (a focused historical replay, not dense synthetic ambient
noise, so `recent_move_rate` should stay low); `layering_score`'s margin
expected to tighten, possibly meaningfully, from a predicted self-
referential risk: in a small, focused replay, the "ambient typical"
baseline could end up dominated by the spoofer's *own* repeated pattern,
partially cancelling the very signal being measured.

**Two real bugs found while verifying, both fixed, both worth recording
in full since the actual mechanism didn't match the prediction in either
case:**

1. **`layering_score` self-contamination — found via a failing existing
   unit test, not Sarao.** `ModerateSignalsWithConcurrentLayeredOrdersFires`
   flipped from firing to not firing. Root cause: the ambient
   `recent_concurrent_samples` pool included the tracked order's *own*
   account — with only one other account (`ACC-BASE`, one sample) against
   SPOOFER's four (S1/L1/L2/L3), the "typical" baseline was dominated by
   SPOOFER's own layering pattern, exactly the self-referential risk
   predicted above, but manifesting in a basic dense unit test, not the
   sparse Sarao replay it was predicted for. Fixed by recording each
   sample's `account_id` and excluding the tracked order's own account
   when computing its baseline — reusing `AccountRegistry::is_related()`'s
   spirit (compare against *others*, not yourself) rather than inventing
   a new mechanism. New regression test:
   `AmbientLayeringBaselineExcludesTrackedOrdersOwnAccount` (an account
   whose own layering is the *only* activity on that side/instrument —
   without the fix, `typical_concurrent` would be computed from its own
   1/2/3 history instead of the correct 0).

2. **`move_score`'s Sarao regression — found via Sarao itself, exactly
   the check this work was scoped to include.** Result before this fix:
   **20/25 fired** (down from 25/25), not the "roughly unchanged"
   prediction. The self-contamination risk predicted for `layering_score`
   turned out to be a non-issue for Sarao specifically (`typical_concurrent`
   was correctly 0 throughout, once fix #1 above was in place — SARAO is
   the only account in the replay, so self-exclusion reduces to "nothing
   to average," matching the original formula exactly). The real
   mechanism was different from what was predicted: `expected_moves_during_dwell`
   scales with the order's own dwell duration, and Sarao's
   historically-documented pattern dwells ~8s (already long enough to
   floor `speed_score` at 0) — even a low ambient rate (1 move recorded
   in the 30s window) produces a real discount over an 8s dwell
   (`(1/30)×8 ≈ 0.267`), which the shorter dwells checked during design
   (0.7s, 2.5s) never surfaced. The 5 missing fires were each wave's
   `concurrent=0` tier (the last layer to cancel, already known to get
   zero layering bonus — Rows 7-8): old `combined=0.667` (comfortable
   margin); new `combined=0.578` (below threshold).

   Fixed by capping `effective_dwell_s` at `slow_time_in_book_ns` (5s) —
   not a new constant, reusing the existing anchor. Reasoning: past that
   point `speed_score` is already floored at 0, so extrapolating the
   move-rate discount further out compounds the same "not fast" signal a
   second time rather than adding new information. Verified against a
   hand calculation *before* touching code: capped, the `concurrent=0`
   tier recomputes to `combined=11/18≈0.611` — clears 0.6 again. New
   regression test:
   `MoveScoreDwellDiscountCappedAtSlowTimeInBookNsNotFullDwell`, built to
   mirror Sarao's exact shape (one recorded move, 8s dwell), asserting
   the capped value and documenting the uncapped value it would have
   produced instead (0.578, below threshold) directly in the test
   comment. All existing unit tests' dwells were already under 5s, so
   none needed adjustment from this specific fix (confirmed, not
   assumed) — only the two failing tests above needed their exact
   expected scores recomputed for the density-normalization itself.

**Post-mortem on the prediction:** partially right, partially wrong, in
informative ways. The *direction* was right for both signals conceptually
(density normalization does something), but the *specific* risk named
(layering self-contamination hurting Sarao) never materialized there —
it hit a controlled unit test instead, for the same underlying reason
(pooled ambient samples with too little other data). The risk that did
hit Sarao (move_score's dwell-duration scaling) wasn't the one predicted
at all. Recorded here deliberately, not just the corrected final state,
because the *miss* is as informative as the catch: predicting from "is
the dataset sparse" reasoning alone, without also deriving the exact
formula's behavior at the dwell durations actually in play, missed the
real mechanism.

**Final verified numbers** (main eval config, full rate sweep, before →
after this section's two fixes; `TP`/`FP` at threshold 0.5):

| rate/sec | 3 | 5 | 7 | 9 | 12 | 16 | 20 | 25 | 75 | 150 | 250.35 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| P before | 0.786 | 0.696 | 0.620 | 0.533 | 0.375 | 0.236 | 0.206 | 0.078 | 0.044 | 0.008 | 0.005 |
| P after | 0.833 | 0.739 | 0.760 | 0.520 | 0.348 | 0.208 | 0.154 | 0.118 | 0.026 | 0.020 | 0.016 |
| FP before | 9 | 14 | 19 | 21 | 30 | 42 | 50 | 71 | 194 | 485 | 854 |
| FP after | 3 | 6 | 6 | 12 | 15 | 19 | 22 | 30 | 75 | 150 | 252 |

Relative precision collapse (rate=3 → rate=250.35): **168.5x before →
53.3x after** — a real, meaningful stability improvement (~3.2x), not a
complete fix. Honest trade-off, not a clean win: recall dropped at nearly
every rate (e.g. TP 33→15 at rate=3) — the density normalization trades
some sensitivity for stability, most clearly beneficial at the high-
density end where the original problem was worst (F1 at 250.35/sec:
0.009→0.024) and net negative at the low end (F1 at rate=3: 0.564→0.323).
Reported in full, not cherry-picked, per CLAUDE.md's Phase 10/11
reporting standard.

**Sarao: 25/25 fired, max score=0.7611** (was 25/25 before this
detector's redesign at all, dipped to 20/25 mid-investigation, restored
by the dwell cap) — confirmed via the same real-pipeline replay this
project has used throughout, not assumed safe from the unit tests alone.

## Deferred: Instrument Liquidity Tiering

**Status: not started. Concrete follow-on, not an implicit gap.**

Row 3 (bid-ask spread) exposed a real limitation that Rows 1-2 didn't
have: real spread behavior is too heterogeneous across names to
represent with one basket-wide table. DE's own p10 (28 ticks) sits
above the aggregate basket's p99 (21 ticks) — the aggregate table
can't produce a DE-realistic spread even in DE's *typical* case, not
just its tail. Compare Row 1 (trade size), where every symbol's own
median (DE=6, RF=98) falls comfortably inside the aggregate table's
own percentile envelope (1-460) — no symbol is structurally excluded
there the way DE is for spread.

DE also stands out on volatility (p50=2.60 vs. near-zero for
AAPL/EBAY/RF — see `intraday_price_volatility.by_symbol` in
`stylized_facts.json`), so this isn't a spread-only quirk: DE's whole
profile (wide spread + higher volatility + smaller trade size, p50=6
vs. the basket's 20) is characteristic of a genuinely less-liquid name.
Grafting DE's spread onto an otherwise AAPL-shaped trade-size/volatility
profile would produce an internally inconsistent synthetic instrument,
not a realistic illiquid one.

**Root cause in code:** `cpp/simulator/instrument_universe.cpp` gives
every synthetic equity instrument (`ACME`, `GLBX`, `NDEX`, `ORCA`,
`PLTX`, `QTRX`, `RHNO`, `STLR`) an **identical** `avg_daily_volume`
(1,000,000) and `tick_size` (0.01). The field to key liquidity tiers off
already exists on `Instrument` — it's just constant today, carrying no
information.

**What Row 3 does instead (interim, honestly scoped):** `kSpreadOffsetPercentiles`
is calibrated to the trade-count-dominant tier only (AAPL/MSFT/XOM/RF —
~77% of the basket's real trade count, all tight-spread, p50 1-3 ticks).
It is documented in `baseline_generator.cpp` as a liquid-tier table, not
a universal one. It will systematically understate spread for any
synthetic instrument meant to represent a DE/HAS/TGT-like name — there
is currently no such distinction in the simulator, so in practice this
means *all* synthetic equity instruments get liquid-tier spread
behavior, uniformly.

**Follow-on work, if/when taken on** (deliberately scoped as its own
pass, not silently folded into Row 3):
1. Introduce real `avg_daily_volume` (and consequently tick-relative
   spread/size behavior) variation across the 8 synthetic equity symbols
   in `instrument_universe.cpp` — e.g. 2-3 explicit liquidity tiers
   rather than a continuous fit, given only 10 real names to draw from.
2. Revisit Rows 1-3 **together**, keyed off each instrument's tier,
   using the `by_symbol` breakdowns already present in
   `stylized_facts.json` (no new WRDS pull needed — the data already
   supports this, it's just not wired in) — bucket the 10 real names
   into tiers (e.g. tight: AAPL/MSFT/XOM/RF; wide: DE/HAS/TGT/JPM/GILD/EBAY)
   and derive one percentile table per tier per row.
3. Re-run Phase 10's evaluation harness afterward and check whether
   detector precision/recall shifts once thin/wide-spread instruments
   are actually exercised — `depth_ratio_at_placement`
   (`spoofing_layering_detector.cpp`) is a size *ratio*, so it may behave
   differently in a thin book even though it isn't a spread-derived
   signal directly.

## What's independent of WRDS entirely (see also `sarao_case.hpp`)

FX and fixed-income baseline flow have no TAQ equivalent — this
calibration pass only ever touches the equity-shaped constants above; FX/
fixed-income keep their current (uncalibrated, clearly-labeled-as-such)
parameters. The CFTC Sarao validation case is real public regulatory
record, not TAQ-derived, and doesn't wait on any of this — see
`cpp/simulator/abuse/sarao_case.hpp` and `cpp/harness/sarao_validation_main.cpp`.
