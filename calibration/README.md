# calibration/

Offline calibration of `simulator/`'s synthetic baseline parameters
against WRDS/TAQ stylized facts (volume clustering, spread behavior,
cancellation rates). Phase 11 of the build guide.

This directory holds calibration **scripts and output parameter values
only**.

**Never commit raw or derived WRDS/TAQ data to this repo, in any form, at
any path.** TAQ access is licensed for offline academic research use, not
redistribution. Confirm exact redistribution terms once WRDS access is
registered, before adding anything here beyond scripts.

## Status: complete

Real WRDS Daily TAQ data was pulled for a 10-symbol equity basket (AAPL,
DE, EBAY, GILD, HAS, JPM, MSFT, RF, TGT, XOM) and calibrated against.
`output/stylized_facts.json` (derived aggregate statistics, over 1.4M+
trades — no underlying rows) holds the real, committed results:

- **Trade size distribution**: p50 = 20 shares, p90 = 100, p99 = 448, mean ≈ 51.5
- **Bid-ask spread**: p50 = 3 ticks, p90 = 7 ticks, mean ≈ 4.06 ticks
- **Order arrival rate proxy**: 16.686/sec/instrument — applied directly as
  `kEmpiricalOrderArrivalRatePerInstrumentPerSecond` in
  `cpp/simulator/simulator.hpp` (rounded to 16.69)
- **Cancellation-rate proxy** (quote-to-trade ratio — TAQ has no
  order-level cancel messages, so this is a documented proxy, not a
  direct pull): mean ≈ 1.46, used to sanity-check the generator's cancel
  rate, not to replace it directly

Applying the calibrated order-arrival rate (an ~83x increase over the old
guessed value) surfaced three real, since-fixed issues downstream — a
`WashTradeDetector` relatedness-check bug, a `MarkingTheCloseDetector`
scenario-scaling gap, and an RNG-stream confound between baseline and
abuse-scenario generation — each investigated and fixed individually
before any threshold was touched. Full before/after numbers in
`PARAMETER_MAPPING.md`.

FX and fixed-income baseline flow have no TAQ equivalent and remain
uncalibrated, clearly labeled as such — see `PARAMETER_MAPPING.md`'s
"What's independent of WRDS entirely" section.

## How this was done (reproducible with fresh data)

1. `PARAMETER_MAPPING.md` maps every stylized fact to the exact
   `simulator/` constant it replaces, what TAQ can and can't actually
   give you (it's a trade-and-quote tape, not an order feed —
   cancellation rate is a proxy, not a direct pull), instrument
   selection, date range, and the real `trades.csv`/`quotes.csv` export
   schema `scripts/compute_stylized_facts.py` expects (WRDS's actual
   column names, confirmed against a real export — `sym_root`/`time_m`/
   `bidsiz`/`asksiz`, not a guessed schema).
2. WRDS was queried via the `wrds` Python package (queries the same
   backend the web UI does, writes the CSVs directly) for `trades.csv` +
   `quotes.csv` in that schema, into this directory (never committed —
   see `.gitignore` below).
3. `scripts/compute_stylized_facts.py trades.csv quotes.csv` wrote
   `output/stylized_facts.json` — the derived numbers only (percentiles,
   spread stats, the QTR cancellation-rate proxy), never the underlying
   rows.
4. `output/stylized_facts.json`'s numbers were hand-applied to the
   `simulator/` constants `PARAMETER_MAPPING.md` lists — deliberately a
   manual step (reviewing "does this number make sense before it goes in
   a constant" mattered more here than automating the edit).
5. The evaluation harness (`cpp/harness/tse_harness_eval`) was re-run
   with the recalibrated generator; what changed (including the three
   bugs the rate change surfaced) is documented in `PARAMETER_MAPPING.md`
   and `cpp/harness/README.md`.

## What didn't wait on WRDS

The build guide's other Phase 11 item — constructing the CFTC Sarao case
as a FIX-replayable scenario and running it through the full live
pipeline — is real public regulatory record, not TAQ-derived data, and
has no dependency on WRDS access at all. See `cpp/simulator/abuse/sarao_case.hpp`
(the scenario, built from the actual CFTC complaint and CFTC's own press
release — every quantitative detail in it is cited, not invented) and
`cpp/harness/sarao_validation_main.cpp` (replays it through the real
`LivePipeline` via the same Kafka-replay machinery Phase 10 built, and
reports honestly whether `SpoofingLayeringDetector` fires and at what
score).

**Already run, real result:** `SpoofingLayeringDetector` fired on **25 of
25** layered sell orders (5 layers × 5 cycles), scores 0.667–0.817,
comfortably clear of the 0.6 `alert_threshold` in every case, zero missed.
`speed_score` was 0.0 on every single alert — the scenario's illustrative
8-second per-layer dwell (chosen before running, not tuned afterward) is
longer than the detector's 5-second `slow_time_in_book_ns` default, the
exact same generator/detector timing mismatch Phase 10 found in the
general-purpose `SpoofingLayeringDetector` evaluation. Every fire here
came entirely from `depth_score` (1.0 — each layer trivially dominates its
own price level, matching the complaint's "exceptionally large") and
`move_score` (the constructed declining-bid mechanic — see the
ILLUSTRATIVE note in `sarao_case.hpp`) plus the layering bonus from
multiple concurrent same-account orders. Run it yourself: `docker compose
up -d kafka && cmake --build build-bench --target tse_sarao_validation &&
./build-bench/cpp/harness/tse_sarao_validation`.

## `.gitignore`

`trades.csv`, `quotes.csv`, and anything under `output/` other than
`output/stylized_facts.json` (the derived numbers, not the underlying
rows) are gitignored — see `.gitignore` in this directory.
