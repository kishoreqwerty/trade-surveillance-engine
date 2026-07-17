# cpp/orderbook/ — price-time priority book: design decisions

Companion to `order_book.hpp`'s and `depth_snapshot.hpp`'s header comments.
This is the narrative version: why the book is shaped the way it is, and how
its correctness is actually proven, not just asserted.

## What this is, and what it deliberately isn't

`OrderBook` is a passive **replica** of an exchange's book, reconstructed
from FIX message flow (`Order` New/Cancel/Replace, `Execution` fills) — it
never itself matches orders. When two orders "cross," that decision was
already made by the real exchange and arrives here as an `Execution` naming
the exact resting `order_id` it filled. This matters for what "price-time
priority" means in this codebase: it's not about *deciding* who trades
first, it's about correctly *maintaining the queue* an exchange's matching
engine would have used, so that depth, priority, and time-in-book are all
correct for what consumes them later (Phase 5's spoofing/layering detector,
Phase 7/8's alert evidence).

## Price-time priority: "time" is processing order, not `timestamp_ns`

Each price level's resting orders live in a `std::list`, appended to
(`push_back`) on arrival and erased from directly (never re-sorted). The
list's own order **is** the priority — the book never sorts by
`timestamp_ns`'s numeric value. This is the direct answer to the "orders at
the exact same price and same timestamp" edge case: two orders that
genuinely carry an identical `timestamp_ns` (not a contrived test
scenario — FIX timestamps have finite resolution and two orders can
legitimately share one) are still correctly tie-broken, because priority
was never derived from the timestamp field in the first place — it's
derived from which `apply()` call happened first. See
`OrderBook.SamePriceSameTimestampOrdersPreserveArrivalOrder` in
`order_book_test.cpp`.

Both `bids_` and `asks_` are the same `std::map<double, std::list<...>>`
type (ascending by price); "best" is a direction, not a comparator: asks
iterate forward from `begin()` (lowest first), bids iterate in reverse from
`rbegin()` (highest first). This avoids splitting every operation into a
bid-flavored and ask-flavored copy over two differently-comparatored map
types.

## Replace semantics: what loses priority, what doesn't

A `Replace` (`OrderStatus::kReplaced`) always mints a new `order_id` —
`orig_order_id` names the resting order being amended, mirroring real FIX's
`OrigClOrdID`/`ClOrdID` split (already used by `OrderCancelRequest`; see the
updated comment on `fix::Order::orig_order_id` in `cpp/fix/types.hpp`,
extended this phase to also cover `kReplaced` since Phase 2 never needed
that case). What happens next depends on what changed, following standard
exchange amend rules:

| Change | Priority | Mechanism |
|---|---|---|
| Price changed (any qty) | **Lost** | Removed entirely, re-inserted at the back of the (possibly new) price level's queue, with this message's own `timestamp_ns` |
| Quantity increased, same price | **Lost** | Same as above |
| Quantity decreased (or unchanged), same price | **Retained** | Mutated in place — same list position — only `qty` and `order_id` change |

The "retained" path still updates the resting order's `order_id` to the new
one (a Replace always changes `ClOrdID`, even when the queue position
doesn't move) — see
`OrderBook.ReplaceThenPartialFillThenCancelWorksThroughIdentityChange`,
which exercises a fill and then a cancel *against the new id*, proving the
old id is fully retired and nothing still resolves through it.

Four scenarios are each tested directly, not just asserted as "should
work": qty-decrease-retains, unchanged-qty-retains, qty-increase-loses,
price-change-loses (even when *combined* with a qty decrease — proving
price alone is sufficient to lose priority, independent of the qty rule).

## Partial fills and cancel-after-partial-fill

An `Execution` reduces the resting order's `qty` in place — same list
position, no priority change, by construction (there's no code path that
would move it). Reaching zero removes it from the book the same way a
Cancel would. Because a partially-filled order keeps its original
`order_id` (only a Replace changes identity, not an Execution), a
subsequent Cancel against that same id resolves correctly without any
special-casing — `OrderBook.CancelAfterPartialFillCancelsExactRemainder`
confirms this is true behavior, not accidental.

## Cancel vs. Replace vs. Execution: differentiated handling of "unknown order_id"

- **Cancel** referencing an order that isn't currently resting (already
  fully filled, already cancelled, a duplicate cancel) is a **silent
  no-op**. This is deliberate: in real order flow, a Cancel racing a fill
  or an earlier Cancel for the same order is an ordinary, expected
  occurrence, not corruption.
- **Replace** and **Execution** referencing an order that isn't resting
  both **throw `std::invalid_argument`**. An amend or a fill against
  something the book has no record of indicates upstream inconsistency —
  in this single-threaded, strictly-ordered-application model there's no
  legitimate race that would produce it, unlike the Cancel case.

This asymmetry is intentional, not an inconsistency — see `apply(Order)`'s
header comment in `order_book.hpp`.

## `DepthSnapshot`: per-order detail, not just aggregates

The architecture doc's one-line description of `DepthSnapshot` ("depth
snapshot capability") doesn't spell out its exact shape, but Phase 4's own
explicit deliverable — testing that price-time priority is actually
correct, and giving Phase 5's spoofing detector usable evidence — requires
observing *queue order*, not just aggregate depth. `PriceLevel` therefore
carries an ordered `orders` vector (FIFO priority order, `orders[0]` has
highest priority) in addition to a denormalized `total_qty`, rather than
exposing only a sum. Without this, the FIFO/priority tests in
`order_book_test.cpp` (same-price-same-timestamp, all four replace
scenarios) would have no way to observe what they're supposed to prove.

`sequence` (a running count of `apply()` calls) and
`last_event_timestamp_ns` are what make "provably consistent with the
update sequence that produced it" a checkable property rather than an
assertion. `DepthSnapshotConsistency.SnapshotAtEveryPrefixMatchesFreshReplay`
is the actual proof: for every prefix length of a 10-event mixed sequence
(new orders, partial fills, a cancel, both replace flavors, across two
price levels), it takes a snapshot from the live book *and* replays that
exact prefix into a **fresh** `OrderBook`, then asserts the two snapshots
are equal in every field (`operator==` on `DepthSnapshot`, not a
spot-check). If `snapshot()` ever missed an update, double-applied one, or
captured stale internal state, this is what would catch it — not just "the
numbers look plausible at one hand-checked point." Two more tests
(`HandVerifiedMidpointSnapshot`, `HandVerifiedFinalSnapshot`) hand-trace the
same 10-event sequence at two points and assert the exact expected book
state field-by-field, independent of `operator==` — so the consistency
proof and the correctness proof don't share a blind spot.

## Four scrutiny questions, answered explicitly

**Level cleanup.** A price level disappears from the book (and `snapshot()`)
entirely once its last resting order is gone — no lingering zero-qty
entries. Both the Cancel path and the full-Execution path route through the
same `remove_resting()`, which erases the level from the map when its queue
empties. Proven directly via `snapshot()` for both paths:
`CancelOfLastOrderAtLevelRemovesTheLevelItself` and
`FullExecutionOfLastOrderAtLevelRemovesTheLevelFromSnapshot`.

**Multi-instrument scope.** `OrderBook` is deliberately single-instrument —
`apply()` throws if handed an event for a different `instrument_id`
(`WrongInstrumentThrows`). No registry/map-of-books-by-instrument exists in
`cpp/orderbook/` yet, and that's intentional: routing events to the right
per-instrument book is Phase 6's live-pipeline wiring concern, not this
engine's — building it now would be implementing later-phase functionality
early.

**Duplicate `order_id` on New.** Throws `std::invalid_argument` rather than
silently overwriting or rejecting-without-signal — a duplicate `ClOrdID` on
a *New* order (as opposed to a Cancel, where a stale reference is an
ordinary race) indicates upstream data corruption or a caller bug, not a
scenario this single-threaded, strictly-ordered model should paper over.
Tested directly: `DuplicateNewOrderIdThrows`.

**Crossed-book invariant.** None is enforced, deliberately. This class is a
passive replica of externally-reported state, not a matching engine — it
never decides whether a book *should* be crossed, only reflects what it's
told. A hand-rolled check that threw on `best_bid >= best_ask` would make
the surveillance system stop observing exactly when something anomalous is
happening upstream (out-of-order delivery, exchange-side issues, or the
market abuse this system exists to detect in the first place) — directly
counter to its purpose. `AcceptsAndAccuratelyReflectsACrossedBookWithoutRejectingIt`
proves the book accepts a crossing New order without throwing and both
sides continue to report accurately. Detecting/flagging a crossed state, if
ever wanted, belongs to a Phase 5 detector reading through `best_price()`,
not to this class.

## Verification

27 orderbook-specific tests (23 `OrderBook`, 4 `DepthSnapshotConsistency`),
all passing, run three ways from fresh builds:

- **Benchmark (Release):** 113/113 project tests pass (1 pre-existing skip —
  `KafkaReplay`, no broker running this session — unrelated to this phase).
- **ASan:** clean.
- **TSan:** clean (this module is intentionally single-threaded this
  phase — see CLAUDE.md's phase discipline, `orderbook/` must be proven
  correct in isolation before Phase 6 wires it into the concurrent live
  pipeline — so this run isn't exercising concurrency, just confirming no
  memory-safety issue trips TSan's own instrumentation).
