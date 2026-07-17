# cpp/ingestion/ — backpressure policy and TSan findings

Companion to `spsc_ring_buffer.hpp`'s header comments — this is the
narrative version: why drop-oldest, what TSan actually caught while
building this (three times now — see Bug 3, found in Phase 6, not this
phase), and what the Kafka replay verification showed.

## Backpressure policy: drop-oldest

Phase 3 requires picking one of block / grow / drop-oldest and justifying
it. Drop-oldest is the right choice here, for three independent reasons:

1. **Blocking risks the FIX session.** The producer side of this queue is
   Phase 2's FIX session thread. If push() blocked when the queue is full,
   a slow consumer (order book updater) would stall FIX message processing
   — including heartbeat responses — risking a session timeout/disconnect
   over something that should be an internal backpressure concern, not a
   protocol-level one.
2. **Growing contradicts the spec.** The architecture doc calls for a
   "fixed-capacity backing array." A growable ring buffer also can't stay
   lock-free in the general case — resizing needs to synchronize against
   whichever thread is mid-read/write of the old backing array.
3. **Kafka is the actual durability guarantee, not this buffer.** The
   architecture's SPSC-plus-Kafka pairing is deliberate: the ring buffer
   optimizes for hot-path latency and freshness of the *live* view: order
   book updates, detector triggers. Kafka (`kafka_producer.hpp`) is what
   makes the system durable and replayable. A dropped element here is not
   a lost element system-wide — it's recoverable from Kafka for replay
   (Phase 10's evaluation harness) even though the live order book missed
   it in real time. Under sustained overload, presenting the *freshest*
   available view (drop-oldest) is more useful for live monitoring than
   presenting an increasingly stale one (which is what drop-newest, the
   other plausible policy, would produce — the buffer would fill with old
   data and reject everything new until the consumer catches up).

Tested directly in `cpp/tests/ingestion/backpressure_test.cpp`: a
deliberately-stalled consumer (both single-threaded and via a genuinely
stalled second thread) proves the queue never blocks, never grows
(`capacity()` is checked to stay constant), and that once the consumer
resumes it sees a coherent, correctly-ordered suffix of the most recent
pushes — not corrupted data, not the wrong elements.

## Three real bugs TSan caught (not theoretical)

Implementing "drop-oldest" for a lock-free SPSC ring buffer turned out to
have three distinct correctness traps. The first two were found by
actually running `cpp/tests/ingestion/two_thread_pipeline_test.cpp` under
`-DENABLE_SANITIZERS=ON -DSANITIZER=thread` in this phase; the third
(below, "Bug 3") was found later, in Phase 6, by a more adversarial
sustained-load test this module's own test suite didn't happen to
construct. All three were found by actually running the code under TSan,
never by reasoning about it, which is exactly why this project treats a
genuine TSan pass as the bar, not "the logic looks right" — and why "TSan
passed on the tests I wrote" is not the same claim as "this is race-free,"
a distinction Bug 3 makes concrete rather than hypothetical. Full detail
in `spsc_ring_buffer.hpp`'s inline comments; the summary:

**Bug 1 — reclaiming the wrong slot.** `push()`'s original drop-oldest path
called the shared claim helper once, assuming that whatever it reclaimed
would be the exact physical slot `push()` was about to overwrite. If the
consumer made concurrent progress between `push()`'s initial read of
`tail_`/`head_` and the reclaim, that assumption could be wrong — the
reclaim could vacate a *different* logical index than the one `push()`
still went on to overwrite unconditionally, leaving the real target slot
still occupied. Fixed by looping the reclaim on a fresh acquire-read of
`head_` until that specific slot is confirmed vacated (`head_ + capacity_
> t`), not just until *some* reclaim succeeded.

**Bug 2 — reading a slot after signaling it was free, not before.** The
shared claim helper (`try_claim_front`) advanced `head_` via a successful
CAS and only *then* read the slot's value out. The CAS's release-store only
carries happens-before guarantees for what's sequenced *before* it in that
thread's program order — the read was sequenced *after*. That opened a
window where a concurrent thread (the other side of the claim race —
`push()`'s reclaim vs. a real `pop()`) could observe the advanced `head_`
via an acquire-load and start overwriting the slot while the original
claimant's read/move was still in flight. First reclaim test run that
reliably forced the drop-oldest path (see the "deterministic, not
speed-dependent" note below) reproduced it immediately:

```
WARNING: ThreadSanitizer: data race
  Read of size 1 ... by thread T1 (producer, push() writing the new value)
  Previous write of size 1 ... by thread T2 (consumer, try_claim_front's
                                              post-CAS move-out of the old value)
```

Fixed by reading a *copy* of the slot's value before attempting the CAS,
not a move, and not after. Copy, specifically, because a move would itself
be a destructive write — if two callers race to read the same slot
speculatively (harmless for a read) and one of those "reads" is actually a
move, the loser has now corrupted data the winner (or, in the non-reclaim
case, nobody) still needed. This costs one extra copy per claim in
exchange for not needing per-slot sequence numbers (the usual, heavier fix
for this class of problem, à la Vyukov's bounded MPMC queue) — a
deliberate trade given `T` here is `std::variant<Order, Execution>`
carrying a handful of short strings, not something copy-expensive.

**Bug 3 (found in Phase 6) — a losing CAS doesn't synchronize with the
winner's later access to the same physical slot.** The Bug 2 fix (read a
copy before attempting the CAS, not after) is correct for the *specific*
race it was written to close — but it left a second, subtler window open
that Phase 3's own tests never happened to exercise: a *losing* claimant's
speculative copy-read (`T candidate = buffer_[h & mask_];`, taken before
its own CAS attempt) can race against a *different*, winning claimant's
*later* write to that exact physical slot, once that slot has wrapped
around to a later logical index. `compare_exchange_weak`'s failure branch
is `memory_order_relaxed` by construction (the second argument to
`compare_exchange_weak`) — a losing CAS does not synchronize-with whatever
the winning thread does afterward, so nothing ordered the loser's
already-completed read against the winner's subsequent write. This is
invisible at Phase 3's scale/timing (a 20ms-staggered, single-producer-
outrunning-a-trivial-consumer test essentially never wraps the buffer
enough times, fast enough, for the window to be hit) but was caught
immediately by Phase 6's `LiveConsumerSustainedLoad.UnpacedBurst...` test —
an unpaced producer flooding a real book-plus-five-detector consumer,
producing ~150,000+ drop-oldest reclaims in a single run:

```
WARNING: ThreadSanitizer: data race
  Write of size 4 ... by thread T1 (push(), spsc_ring_buffer.hpp:112,
                                     writing the new element)
  Previous read of size 4 ... by thread T2 (try_claim_front(),
                                     spsc_ring_buffer.hpp:171, the
                                     speculative pre-CAS copy)
```

Fixed by replacing the whole copy-before-CAS scheme with per-slot
sequence numbers (Vyukov's bounded-queue technique — the "usual, heavier
fix" the Bug 2 writeup explicitly considered and, at the time, judged
unnecessary; Phase 6 is the concrete evidence that judgment was wrong
under sufficiently adversarial load). Each cell now carries its own
`sequence` atomic; a claim only proceeds once the *cell's own* sequence
number confirms it's ready, and a losing claimant only ever touches that
atomic field on the way to retrying — it never reads `cell.data` unless
it's the confirmed winner. This closes the gap structurally rather than
by reasoning about timing: see `spsc_ring_buffer.hpp`'s class comment for
the full derivation, and `cpp/pipeline/README.md` for the before/after
verification (180/180 tests clean across benchmark/ASan/TSan after the
fix, including 5 repeated clean TSan runs of the specific test that found
Bug 3). As a side effect, `T` no longer needs to be CopyConstructible —
only DefaultConstructible and Move{Constructible,Assignable} — since
there's no more copy-before-CAS to require it.

**A dedicated regression test for Bug 3, not just incidental coverage.**
Bug 3 was originally caught by Phase 6's large sustained-load pipeline
test (`cpp/tests/pipeline/live_consumer_sustained_load_test.cpp`) — a
realistic, but not purpose-built, workload. Relying on "the big test
happened to catch it" isn't a real regression guard: a future change back
toward a copy-before-CAS-style design isn't guaranteed to be caught by
that test's specific data/timing shape. `two_thread_pipeline_test.cpp` now
has `TwoThreadPipeline.HighContentionWraparoundRegressionForBug3`,
purpose-built to hit the exact window: capacity 2 (the smallest legal
value), 500,000 unpaced pushes, so the drop-oldest reclaim path — where
Bug 3 lived — runs on nearly every single push, maximizing how many times
per second the losing-read/winning-write handoff actually executes.
Verified clean across 5 repeated TSan runs.

**A second, genuine finding from building that regression test: capacity
1 was silently broken, and nothing had ever exercised it.** Choosing
capacity 2 (the smallest value that lets wraparound reuse a physical slot
without also colliding with normal publish bookkeeping) required checking
whether capacity 1 would work just as well, or better, for maximizing
contention. It doesn't — it's fully broken. At capacity 1, every logical
index maps to the *same* physical cell as its predecessor, so the "just
published index N" sequence marker (`N + 1`) is numerically identical to
the "vacated, ready for index N + 1" marker the very next `push()` checks
for. `push()` would treat unconsumed data as already free and silently
overwrite it — never calling the reclaim path, never incrementing
`dropped_count()` — breaking the `processed + dropped == pushed`
accounting invariant every test in this codebase relies on. This was
**not** caught by TSan or by any failing test: a pre-existing Phase 3 test
(`RejectsNonPowerOfTwoCapacity`) asserted `EXPECT_NO_THROW` for capacity
1, but only checked that *construction* succeeded — no test had ever
actually pushed and popped through a capacity-1 buffer. Found by manual
trace while choosing a capacity for the new regression test, not by
running anything. Fixed by rejecting capacity 1 outright in the
constructor (capacity must now be a power of two **and** at least 2); the
pre-existing test's `EXPECT_NO_THROW(SpscRingBuffer<int>(1))` line was
changed to `EXPECT_THROW`, and a dedicated
`CapacityOneIsRejectedNotJustDiscouraged` test added. This is the one
genuine *behavior change* to a pre-existing Phase 3 test in the whole
Bug 3 investigation. `git diff` confirms the exact, full scope of what
changed in `cpp/tests/ingestion/`: `spsc_ring_buffer_test.cpp` (the
capacity-1 assertion, plus the new dedicated test) and
`two_thread_pipeline_test.cpp` (purely additive — the new
`HighContentionWraparoundRegressionForBug3` test appended after existing
content, zero lines removed). `backpressure_test.cpp` has no diff at all.
`SustainedLoadPreservesOrderingWithNoLossOrCorruption`,
`SustainedLoadWithConsumerKeepingUpDropsNothing`, and both `Backpressure`
stall/resume tests are therefore byte-for-byte unmodified — confirmed by
the diff, not just by them still passing (which unmodified *or* subtly
different code could both achieve).

**A related, non-bug flakiness fix:** the sustained-load test originally
relied on the producer simply outrunning the consumer to force the
drop-oldest path — true under a plain benchmark build, but TSan's
instrumentation overhead doesn't scale both threads' relative speed
uniformly, so the same test occasionally ran with *zero* drops under TSan
and failed an `EXPECT_GT(dropped_count(), 0)` assertion that had nothing
to do with correctness. Fixed by giving the producer a small deterministic
head start (`sleep_for(20ms)` before the consumer's first `pop()`), which
forces the drop path regardless of which build config or machine this
runs on, rather than leaving it to relative thread scheduling luck.

Verified clean (0 warnings) across 5 repeated runs after both fixes, plus
a fresh from-scratch TSan configure/build/test of the whole project
(86/86 tests, 0 races in CTest's full captured log) — the same rigor
`cpp/fix/README.md` applied to the QuickFIX findings.

## Kafka replay determinism — verified against a real broker

`cpp/tests/ingestion/kafka_replay_test.cpp` skips cleanly (not a failure)
if no broker is reachable at `localhost:9092`, since it's an integration
test against `docker-compose.yml`'s Kafka, not a unit test. It was
actually run against a real broker for this phase, not just left to skip
in CI:

```
$ docker compose up -d kafka  # waited for healthy
$ ./ingestion_tests --gtest_filter=KafkaReplay.*
[ RUN      ] KafkaReplay.ReplayedSequenceReproducesBitIdenticalDownstreamState
[       OK ] KafkaReplay.ReplayedSequenceReproducesBitIdenticalDownstreamState (1170 ms)
[  PASSED  ] 1 test.
```

The test produces a deterministic 50-event recorded sequence (mixed
Orders/Executions), flushes to guarantee durability, then replays the
topic from `OFFSET_BEGINNING` **twice**, independently — the actual claim
being verified is not just "replay matches the original" but "replay is
itself reproducible": both replays' concatenated exact-byte encodings
(`event_codec.hpp`'s `encode()`, not a hash) are asserted equal to each
other *and* to the original in-memory sequence. Also re-run under ASan
against the same live broker for real network-path memory-safety coverage
(passed).

## One more honest finding: UBSan noise in librdkafka itself

Running the Kafka replay test under this project's ASan config (which
bundles UBSan — see the root `CMakeLists.txt`'s sanitizer flags) against a
real broker surfaces several UBSan reports, all confirmed to be inside
librdkafka's own C source, not this project's code:

```
rdvarint.h:71       — left shift of negative value
rdkafka_int.h:1384  — signed integer overflow (timestamp arithmetic)
rdhdrhistogram.c:212 — negative shift exponent
rdhdrhistogram.c:168 — left shift of negative value
```

None of these caused a crash or test failure — the replay test still
passed. Unlike the QuickFIX TSan races (`tsan_suppressions.txt`), these
aren't suppressed here: they don't block anything, and adding UBSan
suppressions wasn't asked for in this phase. Flagging them for the same
reason the QuickFIX races were flagged rather than silently fixed —
they're real, they're third-party, and a future contributor re-running
ASan against a live broker shouldn't have to rediscover that on their own.
