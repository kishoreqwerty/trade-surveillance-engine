# CLAUDE.md

This file is read by Claude Code at the start of every session in this repo. Follow it strictly.

## Project

FIX-Native Trade Surveillance Engine — a post-trade market abuse detection system. C++17 core (FIX ingestion, order book, deterministic detectors), a separate Python FastAPI microservice (Isolation Forest anomaly scoring), TimescaleDB persistence, a thin REST API, and a React dashboard.

See `P2_trade_surveillance_engine_architecture.md`, `P2_FIX_Native_Trade_Surveillance_Engine_HLD.md`, and `P2_trade_surveillance_engine_build_guide.md` in this repo for full design. Read all three before starting any phase — the architecture doc has the concrete module/interface structure, the HLD has the decision rationale, the build guide has the phase sequence and "Done when" criteria.

## Build

Two CMake configurations:
- `benchmark` — optimized, no sanitizers
- `debug` — ASan + TSan enabled

```bash
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release
cmake --build build-debug
cmake --build build-bench
```

Python microservice (`ml_service/`) and React dashboard (`dashboard/`) are independent projects with their own build/test tooling — do not fold them into the CMake build.

```bash
docker compose up          # brings up TimescaleDB + Kafka locally
```

## Testing

- C++: GoogleTest, mirrored test structure under `cpp/tests/`
- Python: pytest under `ml_service/tests/`
- Every phase in the build guide has an explicit "Done when" criterion — treat it as the actual acceptance test for that phase, not a suggestion. Do not mark a phase complete or move to the next one until that criterion is genuinely met and verified (test output, sanitizer output, or measured numbers shown, not asserted).

## Phase discipline — read this before writing any code

- Work through the build guide **in order**, one phase at a time. Do not implement later-phase functionality early.
- Correctness before concurrency: `ingestion/`, `orderbook/`, and `detectors/` must each be proven correct in isolation (single-threaded / static-data tests) before Phase 6 wires them into the live, concurrent pipeline.
- The order book (Phase 4) and the spoofing/layering detector (Phase 5) are the highest-scrutiny pieces of this project. Do not under-test them relative to other modules.
- Any concurrency code (the SPSC ring buffer, the live pipeline integration) must pass under ThreadSanitizer specifically before being considered done.
- The ML microservice (`ml_service/`) must never sit on the synchronous hot path. If a change makes the C++ pipeline block on an HTTP call to `ml_service/`, that's a bug, not a valid implementation, even temporarily for testing.

## Data & compliance rules — non-negotiable

- No real exchange connectivity, no real accounts, no real customer data, ever.
- The synthetic generator (`simulator/`) is the primary, shippable dataset. It is the only data source that gets committed to the repo.
- WRDS/TAQ data (used only in `calibration/`, Phase 11) is for offline calibration of the synthetic generator's parameters. **Never commit raw or derived TAQ data to this repo, in any form, at any path.** `calibration/` may only contain scripts and output parameter values, never the underlying data.
- The CFTC Navinder Sarao case (Phase 11 validation) is public regulatory record and safe to construct as a replayable scenario — this is the one exception to "synthetic only," and it's safe specifically because it's public, not proprietary.
- `ground_truth_label` fields exist only in evaluation/replay-mode data paths. Never let this field or its logic leak into anything that looks like a "live production" code path.

## If time runs short

Cut in this order, protecting the phases that carry the real interview story and the widest opportunity breadth: dashboard (Phase 9) first, then ML microservice (Phase 7), then Kafka (fall back to SPSC-only). Never cut the order book (Phase 4), the evaluation harness (Phase 10-11), or the FIX layer (Phase 2) — FIX is the phase this project is named for and the biggest driver of opportunity breadth across mid/small-tier firms.

## Honesty in reporting

When a phase is finished, the report back should include what broke and how it was fixed, not just a final "it works." For Phase 10/11 specifically: report the full precision/recall/F1 sweep, including where detectors trade off against each other — do not cherry-pick the best threshold. If the Sarao validation case (Phase 11) doesn't fire cleanly, report that honestly with your best explanation rather than adjusting thresholds until it does.

## Style

- C++17, no exceptions across the `IDetector` interface boundary (see `ARCHITECTURE.md` §3)
- All timestamps UTC, nanosecond precision, stored as `int64_t` epoch nanos
- Keep `ml_service/` and `dashboard/` fully independent — no shared build tooling, no implicit coupling beyond their documented REST contracts
