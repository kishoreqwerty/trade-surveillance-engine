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

## Instrument selection

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

Two flat files, minimal columns, so `compute_stylized_facts.py` can
consume them without reshaping:

**`trades.csv`** (from TAQ's `CT`/trades table):
```
symbol,date,time,price,size
AAPL,2026-03-03,09:30:01.123,182.45,100
```
`time` in `HH:MM:SS.sss` (or with microseconds — the script tolerates
either), regular-session trades only (exclude pre/post-market, exclude
trades with a non-empty `COND`/condition code that marks it as a non-
standard print — TAQ's trade condition codes flag things like odd-lots,
average-price trades, etc. that would skew the size distribution).

**`quotes.csv`** (from TAQ's `CQ`/NBBO table — use the pre-computed NBBO
table if your WRDS product has one, not raw per-exchange quotes; you want
the *national* best bid/offer, not one venue's):
```
symbol,date,time,bid,ask,bidsize,asksize
AAPL,2026-03-03,09:30:01.050,182.44,182.46,500,300
```

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
| Price offset from mid | `offset_ticks = uniform_int64(0, 3)` | Bid-ask spread distribution | Median/percentile spread (in ticks) from `quotes.csv`, per liquidity tier |
| Fill/partial/cancel/open split | fixed `0.60 / 0.15 / 0.15 / 0.10` | Cancellation-rate proxy (QTR) | No exact map (TAQ has no true cancel rate — see caveat above); use QTR's relative *level* to judge whether 15% cancelled is in the right ballpark, not as an exact replacement |
| Cancel dwell time | `uniform_int64(rng, 500ms, 30s)` | — (no direct TAQ equivalent; same QTR-timing-proxy caveat) | Best effort only |
| `orders_per_second` (`SimulatorConfig::baseline_orders_per_second`) | `5.0` default | Order arrival rate proxy | Trade + quote-update frequency in `trades.csv`/`quotes.csv`, scaled by an assumed order-to-print ratio (document the assumption) |

### `cpp/simulator/abuse/spoofing_layering.cpp` — the one Phase 10 flagged as actually broken

| Constant | Current value | Stylized fact to replace it |
|---|---|---|
| `dwell_ns` formula | `lerp(60s, 0.5s, severity) + jitter(0, 2s)` | Real cancel-to-placement timing (QTR-timing proxy) |
| `SpoofingLayeringConfig::slow_time_in_book_ns` (the *detector's* config, `cpp/detectors/spoofing_layering_detector.hpp`) | `5s` default | Same real timing distribution — **this is the actual bug Phase 10 found**: the generator's `dwell_ns` only drops below the detector's fixed `5s` threshold above severity ≈0.92, so re-deriving both from the *same* real distribution (not independently-guessed constants) is the fix, not just re-tuning one side |

## What's independent of WRDS entirely (see also `sarao_case.hpp`)

FX and fixed-income baseline flow have no TAQ equivalent — this
calibration pass only ever touches the equity-shaped constants above; FX/
fixed-income keep their current (uncalibrated, clearly-labeled-as-such)
parameters. The CFTC Sarao validation case is real public regulatory
record, not TAQ-derived, and doesn't wait on any of this — see
`cpp/simulator/abuse/sarao_case.hpp` and `cpp/harness/sarao_validation_main.cpp`.
