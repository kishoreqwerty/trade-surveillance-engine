#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace tse::ingestion {

// Fixed-capacity, single-producer/single-consumer lock-free ring buffer.
//
// Backpressure policy: drop-oldest. If the producer calls push() while the
// buffer is full, it reclaims (discards) the oldest unconsumed element and
// writes the new one in its place, rather than blocking or growing. See
// cpp/ingestion/README.md for the full justification; in short: blocking
// the producer risks starving FIX session heartbeats (Phase 2's session
// layer needs to keep responding regardless of book-update backlog), and
// growing contradicts the "fixed-capacity backing array" this class is
// specified to be (and can't be done lock-free in general). Drop-oldest
// keeps the live view fresh under overload while Kafka (the durable layer
// behind this buffer — see kafka_producer.hpp) remains the complete,
// replayable record regardless of what this buffer had to discard.
//
// Concurrency design: `head_` and `tail_` are monotonically increasing
// indices (never reset, only ever masked with `mask_` when indexing into
// storage). The producer is the only writer of `tail_` in the common case;
// the consumer is the only writer of `head_` in the common case. The one
// case where both sides could touch the same slot — the producer needing
// to reclaim the oldest element when full — is arbitrated by a
// compare_exchange loop on `head_` shared between push()'s reclaim path and
// pop() (see try_claim_front()): whichever side's CAS wins is the only one
// that ever touches that slot's value, which is what gives this a genuine
// happens-before edge (not just an atomic-index guarantee).
//
// That CAS arbitration is necessary but not sufficient on its own: push()
// must keep reclaiming (looping on a fresh acquire-read of head_, not just
// attempting a single reclaim) until head_ has specifically moved past the
// logical index that maps to the slot it's about to overwrite — a single
// reclaim can claim a *different* front element than the one occupying
// that slot if the consumer makes concurrent progress in between. An
// earlier version of this class got that wrong (reclaimed once and assumed
// it was the right slot) and TSan caught the resulting race under
// sustained two-thread load — see cpp/ingestion/README.md and
// cpp/tests/ingestion/two_thread_pipeline_test.cpp, which is what
// reproduces it.
//
// A second, subtler bug in the same area (also TSan-caught, also detailed
// in the README) was try_claim_front() moving a slot's value out *after*
// winning the CAS on head_ instead of *before* — see that function's
// comment for why the ordering matters and why the fix requires T to be
// CopyConstructible (in addition to DefaultConstructible, needed for the
// backing array), not just MoveConstructible.
//
// Now genuinely TSan-clean under real concurrent load.
template <typename T>
class SpscRingBuffer {
public:
    // capacity must be a power of two (enables index masking instead of
    // modulo, and makes the "is full" check exact). Not rounded up
    // automatically — an unexpected capacity should be visible at
    // construction, not silently changed.
    explicit SpscRingBuffer(std::size_t capacity)
        : capacity_(capacity), mask_(capacity - 1), buffer_(std::make_unique<T[]>(capacity)) {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("SpscRingBuffer: capacity must be a power of two");
        }
    }

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    // Producer-only. Never blocks. Returns true if the element was added
    // without dropping anything; false if the buffer was full and the
    // oldest unconsumed element was discarded to make room (see
    // dropped_count() for a running total).
    bool push(T item) {
        std::size_t t = tail_.load(std::memory_order_relaxed);
        bool dropped = false;

        // The slot we're about to write (t & mask_) was last used for
        // logical index (t - capacity_); it's only safe to overwrite once
        // the consumer has moved past that specific index, i.e. once
        // head_ > t - capacity_ (written as head_ + capacity_ > t to avoid
        // unsigned underflow when t < capacity_, which is the common case
        // before the buffer has filled once).
        //
        // A single reclaim via try_claim_front() is NOT sufficient here:
        // try_claim_front() reclaims whatever the front element currently
        // is, and if the consumer makes concurrent progress between our
        // read of `t` and the reclaim, that can be a *different* logical
        // index than (t - capacity_) — leaving our actual target slot
        // still occupied by an unconsumed element the consumer might be
        // concurrently reading. (This was a genuine bug caught by TSan
        // under sustained load, not a theoretical concern — see
        // cpp/ingestion/README.md.) Looping on a fresh acquire-read of
        // head_ after every reclaim attempt is what actually guarantees
        // slot (t & mask_) specifically has been vacated before we touch
        // it.
        while (head_.load(std::memory_order_acquire) + capacity_ <= t) {
            T discarded;
            if (try_claim_front(discarded)) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                dropped = true;
            }
            // If the claim failed, a concurrent pop() claimed that front
            // element first — which is exactly the progress we needed;
            // loop and re-check with a fresh head_ read.
        }

        buffer_[t & mask_] = std::move(item);
        tail_.store(t + 1, std::memory_order_release);
        return !dropped;
    }

    // Consumer-only (may be called concurrently with push(), including
    // push()'s drop-oldest reclaim — see try_claim_front()). Returns true
    // and fills `out` if an element was available.
    bool pop(T& out) { return try_claim_front(out); }

    std::size_t capacity() const { return capacity_; }

    // Approximate — for metrics/tests only. Reading two independently
    // updated atomics without a lock cannot give an exact size under
    // concurrent access; that's fine here since nothing relies on it being
    // exact.
    std::size_t size_approx() const {
        std::size_t t = tail_.load(std::memory_order_acquire);
        std::size_t h = head_.load(std::memory_order_acquire);
        return t - h;
    }

    std::uint64_t dropped_count() const { return dropped_.load(std::memory_order_relaxed); }

private:
    // Shared claim protocol for "take exclusive ownership of the front
    // element and remove it." Used by both pop() (the common case) and
    // push()'s drop-oldest reclaim (the rare, full-buffer case). Exactly
    // one caller can ever win the CAS for a given index value, so exactly
    // one caller ever *commits* that slot's value as its result — but see
    // the copy-before-CAS comment below for why that alone isn't enough.
    bool try_claim_front(T& out) {
        std::size_t h = head_.load(std::memory_order_relaxed);
        for (;;) {
            std::size_t t = tail_.load(std::memory_order_acquire);
            if (h == t) return false;  // empty, nothing to claim

            // Read a COPY before attempting the claim — not a move, and
            // not after a successful CAS. An earlier version of this
            // function did `out = std::move(buffer_[h & mask_])` *after*
            // the CAS succeeded, which TSan caught as a real race under
            // sustained load: the CAS's release-store only carries
            // happens-before guarantees for operations sequenced *before*
            // it in this thread's program order, and that move was
            // sequenced *after*. A concurrent thread that acquire-loads
            // head_ (e.g. push()'s drop-oldest reclaim loop) could observe
            // the slot as vacated and start overwriting it while this
            // move was still in flight.
            //
            // Reading here, before the CAS, fixes the ordering — but it
            // must be a copy, not a move: two callers can race to read the
            // same slot speculatively (harmless, concurrent reads), and
            // exactly one wins the CAS and keeps its copy; but if either
            // read were a destructive move, the *other* (losing) caller's
            // read would have been a real write to the same memory,
            // racing the winner's read. Copying costs one extra
            // construction on every claim, in exchange for a design that
            // doesn't need per-slot sequence numbers (Vyukov-style) to be
            // correct. See cpp/ingestion/README.md.
            T candidate = buffer_[h & mask_];

            if (head_.compare_exchange_weak(h, h + 1, std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
                out = std::move(candidate);
                return true;
            }
            // Lost the claim to a concurrent caller (pop() vs. push()'s
            // reclaim, racing for the same slot). `candidate` is simply
            // discarded — it was a copy, so nothing was mutated by reading
            // it. `h` was refreshed to the current head_ value by
            // compare_exchange_weak itself; loop and retry against it.
        }
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<T[]> buffer_;

    alignas(64) std::atomic<std::size_t> head_{0};  // consumer-owned index (cache-line separated
    alignas(64) std::atomic<std::size_t> tail_{0};  // producer-owned index  from head_ to avoid
                                                     // false sharing under sustained load)
    std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace tse::ingestion
