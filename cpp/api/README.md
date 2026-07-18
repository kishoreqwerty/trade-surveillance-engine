# cpp/api/ — the REST layer: design decisions and real numbers

Companion to `P2_trade_surveillance_engine_architecture.md`'s module table.
This is the narrative version: why Crow over Drogon, how a browser-only bug
and a genuine TSan race both got caught, and the live-server entrypoint
this phase's "Done when" actually needs.

## Crow over Drogon

The build guide names both as acceptable. Drogon pulls in its own
ORM/WebSocket/coroutine machinery this project doesn't need — `db/`
already is the persistence layer, and the architecture doc's data flow is
explicitly "pull, not push, for v1" (no WebSocket push needed). Crow's
plain HTTP JSON routing is the closer match to this project's existing
dependency-minimalism precedent (`cpp-httplib` chosen for `ml_client/`
over heavier alternatives for the same reason). Crow needs standalone Asio
(not Boost.Asio) to build — header-only, fetched the same way, no build
step of its own (`FetchContent_Populate`, not `MakeAvailable`, since
there's no `CMakeLists.txt` to `add_subdirectory()` into).

## The endpoint contract

| Method | Path | Notes |
|---|---|---|
| GET | `/api/health` | liveness check |
| GET | `/api/alerts` | `?account_id=` / `?detector_name=` / `?start_ns=&end_ns=` (mutually exclusive, checked in that priority order) / `?limit=` (default 50, no filter). Maps directly onto `AlertStore`'s Phase 8 query methods — one filter per request, not a combinator. |
| GET | `/api/alerts/<id>` | 404 if unknown, 400 if `<id>` isn't an integer |
| PATCH | `/api/alerts/<id>/status` | body `{"status": "..."}`. Valid values enforced by `schema.sql`'s CHECK constraint (not re-validated here — see `AlertStore::update_alert_status()`'s own comment on why duplicating that list would just be a second place for it to drift) |
| GET | `/api/orderbook/<instrument>/snapshot` | 503 if no live pipeline is attached to this process, 404 if the instrument has never traded |

## `LiveBookRegistry`: the thread-safety layer this phase adds

`LivePipeline` (Phase 6) is deliberately, explicitly not thread-safe — its
own header comment says "exactly one consumer thread ever calls
process()." That was fine through Phase 8: every caller was a test or a
`LiveConsumer` running on its own thread, joined before anything else
touched the result. Phase 9 is the first time a *second* kind of caller —
HTTP handler threads, via `GET /api/orderbook/.../snapshot` — needs to
read book state while a live pipeline is still running concurrently.

`LiveBookRegistry` (`live_book_registry.hpp/.cpp`) is a small,
purpose-built wrapper: the same ring-buffer pop-and-process shape as
`LiveConsumer`, but every `LivePipeline::process()` call and every
`snapshot()` read share one mutex. Deliberately not built by modifying or
templating `LiveConsumer` itself — three earlier phases' tests already
depend on its exact current shape, and duplicating a dozen lines here is
lower-risk than changing a class this project has already built three
phases of proof on top of.

## A real TSan-caught data race, found building this phase's own test

`LiveBookRegistry::events_processed()`/`events_skipped_inconsistent()`
were originally plain `uint64_t` counters, incremented by the consumer
thread inside `process_one()`. Every existing precedent for this shape of
counter (`LiveConsumer`'s own equivalents) is only ever read *after*
joining the thread that increments it. This phase's own
`ApiServerOrderBookTest.ReturnsRealBookStateAfterAnOrderIsProcessed` test
does something none of those did: it polls `events_processed()` from the
main thread *while* the consumer thread is still running, waiting for a
just-pushed event to be reflected. TSan caught the resulting race
immediately:

```
WARNING: ThreadSanitizer: data race (pid=18449)
  Write of size 8 ... by thread T1: LiveBookRegistry::process_one
  Previous read of size 8 ... by main thread: LiveBookRegistry::events_processed
```

Fixed by making both counters `std::atomic<uint64_t>` — a one-line-per-field
change, verified by rerunning the exact test that caught it (clean) and the
full suite (249/249, 0 races, confirmed by reading the log content, not
just the exit code).

## A real bug only a browser catches: CORS

Every route-level test in `cpp/tests/api/` — the entire `ApiServerTest`
and `ApiServerOrderBookTest` suites — passed against this API before a
single line of CORS handling existed, because they all use `cpp-httplib`'s
client, which (like `curl`) doesn't enforce CORS. The first real headless-
Chromium run against the actual dashboard (`dashboard/`, Vite dev server on
`:5173`) hit every endpoint and failed silently in a way no HTTP-client-based
test could ever catch:

```
Access to fetch at 'http://127.0.0.1:8081/api/alerts?limit=100' from origin
'http://localhost:5173' has been blocked by CORS policy: No
'Access-Control-Allow-Origin' header is present on the requested resource.
```

Fixed with Crow's built-in `CORSHandler` middleware (`crow::App<crow::CORSHandler>`,
aliased as `tse::api::App`), configured permissively (this is a local,
unauthenticated dev API — the same posture this project already takes with
Kafka/TimescaleDB) but with `Content-Type` and the actual methods used
listed explicitly rather than `"*"` — Crow's own header comment on the
middleware warns that a wildcard `Access-Control-Allow-Headers` gets
ignored by a browser's OPTIONS-preflight cache, which would have silently
broken the PATCH endpoint specifically (it's the only one sending a JSON
body). Re-verified with the same headless-Chromium script afterward: zero
console errors, 36 real alert cards rendered, a real PATCH round-trip
(button click → visible status-pill change) captured in a before/after
screenshot pair.

This is the concrete version of what the system prompt's "start the dev
server and use the feature in a browser" instruction is for: neither the
21 route-level tests nor a decade of `curl` scripting would ever have
caught this, because none of them are a browser.

## The live demo server (`cpp/api/main.cpp`) — Phase 9's actual "Done when"

The build guide's "Done when" is explicit: "the dashboard, running
against the live pipeline, correctly displays alerts as they're
generated." That needs a real, long-running process — the first one this
project has built (Phases 1-8 are all gtest binaries or one-shot tools).
`main.cpp` wires:

- A real FIX 4.2 loopback acceptor+initiator session — the production
  version of `cpp/tests/fix/fix_session_test_fixture.hpp`'s setup, reused
  unmodified in spirit (same `SurveillanceFixApplication`, same
  `SessionSettings` shape).
- A background thread looping `tse::simulator::generate_simulation()` at a
  fixed, deterministic instrument/account universe (so the dashboard
  always has the same symbols to watch across loops), sending every
  Order/Execution as a real FIX message via `FIX::Session::sendToTarget`,
  paced 15ms apart so arrival is visibly gradual rather than instant.
- The acceptor's `IEventSink` hook feeding `pipeline/`'s ring buffer, drained
  by `LiveBookRegistry` into a `LivePipeline` running all six detectors
  (five synchronous plus `MlAnomalyDetector`, which fails open harmlessly
  if `ml_service/` isn't running — Phase 7's own proven behavior).
- `DbAlertSink` writing every resulting alert straight to the same
  TimescaleDB `docker-compose.yml` brings up.
- Crow serving the REST API in the foreground, `app.run()` blocking for
  the process's lifetime.

Run it with `docker compose up -d timescaledb` first, then
`./build-bench/cpp/api/tse_api_server [port]` (default 8081).

## Verification

**21 tests** (`cpp/tests/api/`): `json_encode_test.cpp` (6, pure unit,
encode/decode round-trips including null-optional-field handling),
`live_book_registry_test.cpp` (2, including the concurrent-reader-vs-writer
TSan proof above), `api_server_test.cpp` (13, real HTTP requests via
`cpp-httplib` against a real running Crow app and a real TimescaleDB —
every filter combination, every error path, both the nullptr- and
real-`LiveBookRegistry` order-book cases).

| Config | Result |
|---|---|
| Benchmark | 249/249 passed |
| ASan | 249/249 passed, 0 errors |
| TSan | 249/249 passed, 0 data races (after the fix above) |

Plus the live-pipeline proof no unit test can substitute for: the real
demo server, driven by a real headless Chromium (`playwright`, installed
ad hoc for this verification, not a project dependency), showed 36 live
alert cards, a ticking order-book sequence number, and a real compliance-
action button click that changed a real alert's status in TimescaleDB —
captured as a before/after screenshot pair.
