# cpp/ingestion/ — backpressure policy and TSan findings

Companion to `spsc_ring_buffer.hpp`'s header comments — this is the
narrative version: why drop-oldest, what TSan actually caught while
building this (twice), and what the Kafka replay verification showed.

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

## Two real bugs TSan caught (not theoretical)

Implementing "drop-oldest" for a lock-free SPSC ring buffer turned out to
have two distinct correctness traps, both found by actually running
`cpp/tests/ingestion/two_thread_pipeline_test.cpp` under
`-DENABLE_SANITIZERS=ON -DSANITIZER=thread` — not found by reasoning about
the code, which is exactly why this phase treats a genuine TSan pass as
the bar, not "the logic looks right." Both are explained in more detail in
`spsc_ring_buffer.hpp`'s inline comments; the summary:

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
