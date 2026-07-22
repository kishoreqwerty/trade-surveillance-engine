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

## Status

Waiting on WRDS account approval. Everything that can be prepared without
real data is done; nothing here has touched real TAQ data yet.

## Once WRDS access is approved

1. Read `PARAMETER_MAPPING.md` first — it maps every stylized fact this
   phase needs to the exact `simulator/` constant it replaces, what TAQ
   can and can't actually give you (it's a trade-and-quote tape, not an
   order feed — cancellation rate is a proxy, not a direct pull), instrument
   selection, date range, and the exact `trades.csv`/`quotes.csv` export
   schema `scripts/compute_stylized_facts.py` expects.
2. Query WRDS (the `wrds` Python package is the smoothest path — queries
   the same backend the web UI does, writes the CSVs directly) and export
   `trades.csv` + `quotes.csv` in that schema, into this directory (they
   will not be committed — see `.gitignore` below).
3. Run `scripts/compute_stylized_facts.py trades.csv quotes.csv` — writes
   `output/stylized_facts.json`, the actual numbers (percentiles, spread
   stats, the QTR cancellation-rate proxy), never the underlying rows.
4. Hand-apply `output/stylized_facts.json`'s numbers to the `simulator/`
   constants `PARAMETER_MAPPING.md` lists — deliberately a manual step
   (reviewing "does this number make sense before it goes in a constant"
   matters more here than automating the edit).
5. Re-run Phase 10's evaluation harness (`cpp/harness/tse_harness_eval`)
   with the recalibrated generator and document whether/how the numbers
   changed in `cpp/harness/README.md` — per the build guide, this is
   required, not optional, and "nothing changed" is a valid, reportable
   outcome if that's what happens.

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
