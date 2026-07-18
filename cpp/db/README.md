# cpp/db/ — the alert store: design decisions and real numbers

Companion to `P2_trade_surveillance_engine_architecture.md`'s "Alert
evidence contract" and module-responsibility table. This is the narrative
version: why libpq is built from source, why the store writes
synchronously, and the actual proof against a real TimescaleDB instance
the build guide's Phase 8 "Done when" asks for.

## Why libpq is built from source

Every other third-party dependency in this project (QuickFIX, librdkafka,
cpp-httplib, googletest) is a plain CMake `FetchContent_Declare` +
`FetchContent_MakeAvailable` — no system packages required, matching
CLAUDE.md's Build section, which lists exactly two `cmake` invocations and
nothing about installing anything first.

libpqxx can't be handled the same way: it needs libpq (the Postgres C
client library) to link against, and Postgres itself has no CMake build of
its own — `./configure && make` only. The machine this was developed on has
no Homebrew, MacPorts, Postgres.app, or system `pg_config`, so there was no
libpq to `find_package()` out of the box, and requiring every future clone
of this repo to install a system package manager first isn't a prerequisite
CLAUDE.md's Build section asks for.

So the root `CMakeLists.txt` clones the official Postgres source
(`REL_16_9`, matching `docker-compose.yml`'s `timescale/timescaledb:*-pg16`
image) and builds *just* the client library — `./configure`, then
`make -C src/include install` (the shared public headers libpq-fe.h itself
`#include`s, like `postgres_ext.h` — genuinely required, found by a real
build failure, not assumed up front) and `make -C src/interfaces/libpq
install` — never the server, never the full `make install`. This runs once
per build directory, at configure time (blocking, a few minutes); the
`EXISTS` check at the top of that CMakeLists.txt block means every later
`cmake` invocation against the same build directory skips straight past it.

One more real finding, not assumed: `libpq.a` alone is missing symbols
(`pg_strcasecmp`, `scram_exchange`, `pg_saslprep`, ...) that only live in
`libpgcommon.a`/`libpgport.a` — Postgres's frontend-shared string/auth/port
helper code. A normal system package only ships `libpq.so` (which embeds
the position-independent variants of these internally), so this only
surfaces when statically linking, which is what this project does
everywhere else (QuickFIX, librdkafka, httplib are all static too — see
each one's section in the root `CMakeLists.txt`). Those two archives get
copied out of the build tree and linked into `tse_db` explicitly.

The dylib libpq's own `make install` also produces is deleted immediately
after build — deliberately, so `find_package(PostgreSQL)` (which libpqxx's
own `CMakeLists.txt` calls) resolves to the static archive instead of
leaving test binaries with an untracked runtime dylib dependency.

## Why `AlertStore` writes synchronously, not through a bounded queue

`ml_client/`'s `MlScoringWorker` (Phase 7) exists specifically because a
blocking network call has no business on `LiveConsumer`'s hot thread.
`AlertStore::insert_alert()` is also a blocking network call, called from
`DbAlertSink::on_alert()` — so why isn't it wrapped in the same
bounded-queue-plus-worker-thread pattern?

Because nothing in this codebase calls it from the hot thread yet. There is
no live production entrypoint that wires `DbAlertSink` into
`LiveConsumer`'s actual alert sink — Phase 6/7's tests use
`CollectingAlertSink`, and this phase's own integration test
(`db_alert_sink_integration_test.cpp`) calls `LivePipeline::process()`
directly and forwards its results to `DbAlertSink` from the test's own
thread, not from a running `LiveConsumer`. Building the async version of
this now would be solving a problem that doesn't exist yet, based on a
guess about what a not-yet-built entrypoint's throughput requirements will
turn out to be — exactly the kind of premature abstraction CLAUDE.md's
project instructions warn against.

If and when a live entrypoint does wire `DbAlertSink` onto the hot
consumer thread, `MlScoringWorker`'s shape (bounded
`ingestion::SpscRingBuffer`, drop-oldest, dedicated writer thread) is the
obvious template — reusing the same, already-hardened queue class, not
inventing a second implementation of the same backpressure policy. That's a
decision for whoever builds that entrypoint, with real throughput numbers
in hand, not one to make speculatively here.

## Schema shape

Three hypertables (`orders`, `trades`, `alerts`), each partitioned on an
`event_time TIMESTAMPTZ` column computed from that row's own authoritative
nanosecond timestamp at INSERT time (`timestamp_ns` for orders/trades,
`window_end_ns` for alerts) — not wall-clock insertion time. This keeps
partitioning fully deterministic under replay/test conditions, independent
of when a row happens to actually be written.

`alerts.account_ids`/`alerts.order_ids` are native Postgres `TEXT[]`
columns, not a join table — an alert can implicate more than one account
(`WashTradeDetector`'s two related accounts) or order
(`FrontRunningDetector`'s leader+follower pair), and the query surface only
ever needs "does this alert involve account X," which a GIN-indexed array
containment check (`account_ids @> ARRAY[$1]`) answers directly.
`model_version` and `book_snapshot_sequence` are nullable — see below.

`schema.sql` is the single source of truth, applied via
`AlertStore::apply_schema()` (every statement is `IF NOT EXISTS`, safe to
call on every startup, real or test) rather than duplicated as inline
C++ string literals.

## `book_snapshot_sequence`: a gap this phase found and closed

The architecture doc's Alert evidence contract has always named a "book
depth snapshot reference" as part of what every persisted alert should
carry. Before this phase, no detector actually populated one —
`detectors::Alert` had no field for it at all. `OrderBook::sequence()`
(Phase 4) already exists specifically for this: a running count of
`apply()` calls, cheap to read, enough (together with `instrument_id`) to
replay a book to the exact state a detector was looking at without storing
a full `DepthSnapshot` in every alert row.

This phase added `Alert::book_snapshot_sequence` (`std::optional<int64_t>`)
and wired it into all six alert-firing paths: all five synchronous
detectors call `book.sequence()` at the point they construct their `Alert`
(three of them — `WashTradeDetector`, `StatisticalBaselineDetector`, and
`SpoofingLayeringDetector`'s `handle_cancel` — already had `book` named in
scope; `MarkingTheCloseDetector` and `FrontRunningDetector` needed it
threaded through a private helper that didn't previously take it).
`MlAnomalyDetector` is the interesting case: its `evaluate()` never blocks,
so the actual `Alert` is built later, on `MlScoringWorker`'s separate
background thread, which has no `OrderBook` reference at all. Fixed by
adding `book_snapshot_sequence` to `ScoringRequest` — captured at
submission time in `evaluate()`, carried through (not part of the wire
payload to `ml_service/`, purely local bookkeeping), and copied onto the
resulting `Alert` in `MlScoringWorker::process_one()`.

This is the same kind of gap as `model_version` (Phase 7's structured
field, added after a direct question about whether it was captured or
silently discarded) — evidence the architecture doc had always promised,
that no code actually produced until something forced the question.

## Postgres array + parameter-type quirks, found against the real database

Two real bugs surfaced only once tests ran against an actual TimescaleDB
instance, not before:

**Array encode/decode.** libpqxx has no built-in `std::vector<std::string>`
↔ `TEXT[]` binding. `to_pg_text_array()`/`from_pg_text_array()`
(`alert_store.cpp`, anonymous namespace) hand-roll it — matching the
project's existing "hand-roll exactly the encoding a fixed, known shape
needs" precedent (`ml_client/ml_json.cpp`,
`simulator/serialization/json_writer.cpp`). Decode has to handle both
quoted and unquoted array elements: Postgres's own `array_out` only quotes
elements that need it (special characters, empty string), so a value like
`{ACC-1,ACC-2}` comes back unquoted while `{"ACC,2","ACC\"3"}` comes back
quoted — `alert_store_test.cpp`'s round-trip test deliberately includes an
account ID containing a comma and one containing a double-quote, not just
plain alphanumeric IDs, so this path is actually exercised both ways.

**Parameter type inference.** Every INSERT reuses one bind parameter twice
— once bound straight to a `BIGINT` column, once inside
`to_timestamp($n::double precision / ...)` to derive `event_time`. The
first version left one of the two occurrences uncast and failed against
the real database with `ERROR: inconsistent types deduced for parameter
$n: double precision versus bigint` — Postgres's parameter type inference
tries to unify a parameter to one type across an entire statement, and
disagrees the moment two occurrences imply different types. None of the
unit-level tests caught this (there was no real Postgres to run the actual
`INSERT` against) — only the integration run against `docker-compose`'s
TimescaleDB did. Fixed by giving both occurrences an explicit cast
(`$n::bigint` for the column, `$n::double precision` for the timestamp
conversion) rather than relying on either being inferred from context.

## Verification

**12 tests, all against a real TimescaleDB instance** (`docker compose up
-d timescaledb`), none mocked:

- `alert_store_test.cpp` (5 tests): schema apply is idempotent (called
  twice, no error); `insert_order`/`insert_execution` succeed against the
  real schema; `insert_alert` round-trips every field exactly, including
  account IDs containing a comma and a double-quote (the array-escaping
  stress case above); `model_version`/`book_snapshot_sequence` round-trip
  as real SQL `NULL` → unset `std::optional`, not a sentinel value, when a
  hand-constructed `Alert` leaves them unset.
- `alert_store_query_test.cpp` (6 tests): the three query shapes the build
  guide's Phase 8 "Done when" names explicitly — time-range,
  filter-by-account, filter-by-detector-type — each proven against a fixed
  set of four alerts built so every query has both a row that must be
  included and a row that must be excluded (a query that degenerated into
  "return everything" fails these as loudly as one returning nothing), plus
  a no-matches-returns-empty case for each.
- `db_alert_sink_integration_test.cpp` (1 test): the actual "Done when" —
  a real `Alert`, produced by driving a real `LivePipeline` +
  `WashTradeDetector` through the same New→Execution scenario
  `cpp/tests/pipeline/live_pipeline_test.cpp` already proved fires
  correctly, written through a real `DbAlertSink`, then read back through
  `AlertStore`'s query layer and checked field-for-field against the
  *original* fired `Alert` — including `book_snapshot_sequence`, proving
  this phase's new field survives the real round trip, not just a
  hand-constructed fixture's.

Every test connects via `connect_or_skip()` (`db_test_helpers.hpp`) and
`GTEST_SKIP()`s — doesn't fail — if TimescaleDB isn't reachable, matching
this project's Kafka-broker-skip precedent
(`cpp/tests/ingestion/kafka_replay_test.cpp`): these are integration tests
against a real external service, not pure unit tests, and "the service
isn't running in this environment" isn't a test failure.

Confirmed clean across all three sanitizer configs (benchmark/ASan/TSan),
run sequentially per this project's established QuickFIX-shared-lib-race
discipline — see the top-level session report for the actual numbers.
