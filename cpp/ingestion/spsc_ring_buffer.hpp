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
// Concurrency design: Vyukov-style per-slot sequence numbers, not raw
// head_/tail_ index comparisons around a shared data array. This is a
// Phase 6 rewrite, replacing a Phase 3 design (copy-the-value-before-CAS)
// that was proven wrong by Phase 6's own sustained-load test, not a
// preemptive rewrite — see "Why the Phase 3 design wasn't enough" below.
//
// Each Cell carries its own `sequence` atomic alongside its `data`. A
// producer targeting logical index `pos` only writes once
// `cell.sequence.load(acquire) == pos` (the slot has been vacated for
// exactly this position, not just "some" position), then publishes via
// `cell.sequence.store(pos + 1, release)`. A consumer (or push()'s
// drop-oldest reclaim — see try_claim_front(), used by both) only reads
// once `cell.sequence.load(acquire) == pos + 1` (the producer's publish is
// visible), then frees the slot for the *next* wraparound via
// `cell.sequence.store(pos + capacity_, release)`. head_/tail_ still exist
// as monotonic position counters for arbitrating *which* logical index
// each side is working on (via a CAS on head_, exactly as before, shared
// between pop() and push()'s reclaim path) — but they no longer carry the
// data-safety synchronization themselves; the per-cell sequence does that.
//
// Why the Phase 3 design wasn't enough: it relied on a losing CAS attempt
// in try_claim_front() being harmless because the loser only read a copy,
// never wrote. That's true for the *logical* index the loser and winner
// were both contending for — but it missed a second scenario: a claim
// attempt's speculative read (`T candidate = buffer_[h & mask_]`, taken
// *before* attempting the CAS) can race against a *different* thread's
// write to that same *physical* slot for a *later* logical index, once
// that later index has wrapped around to reuse the slot. A losing
// `compare_exchange_weak` only provides `memory_order_relaxed` semantics
// on failure — it does not synchronize-with whatever the *winning* thread
// does afterward, so nothing ordered the loser's already-completed read
// against the winner's later write to that physical slot. This is exactly
// what TSan caught in Phase 6's sustained-load test under real,
// high-volume drop-oldest churn (a scenario Phase 3's own smaller/paced
// tests didn't happen to trigger): a write in push() (spsc_ring_buffer.hpp,
// old line 112) racing a read in try_claim_front() (old line 171). See
// cpp/pipeline/README.md for the full before/after story. Per-slot
// sequence numbers close this gap structurally: a losing claimant in the
// new design only ever reads the atomic `sequence` field (always race-free
// by definition) before deciding to retry — it never touches `cell.data`
// unless it's the confirmed winner, and by the time it's the winner, the
// per-cell acquire/release pair has already established the happens-before
// edge that makes the read or write safe.
//
// T must be DefaultConstructible (Cell's `data` member) and
// MoveConstructible/MoveAssignable. Unlike the Phase 3 design, T no longer
// needs to be CopyConstructible — there is no more copy-before-CAS; the
// sequence number does that job instead.
template <typename T>
class SpscRingBuffer {
public:
    // capacity must be a power of two >= 2 (enables index masking instead
    // of modulo, and makes the "is full" check exact). Not rounded up
    // automatically — an unexpected capacity should be visible at
    // construction, not silently changed.
    //
    // capacity == 1 is rejected, not just an unlikely-to-be-useful choice:
    // at capacity 1, every logical index maps to the same physical cell as
    // its immediate predecessor, so the "just published, index N" marker a
    // producer writes (sequence == N + 1) is numerically indistinguishable
    // from the "vacated, ready for index N + 1" marker the very next
    // push() checks for (also N + 1, since mask_ == 0 means index N + 1's
    // check target is itself N + 1). push() would then treat unconsumed
    // data as already-free and silently overwrite it without ever calling
    // try_claim_front() or incrementing dropped_count() — breaking the
    // processed + dropped == pushed accounting invariant every test in
    // this codebase relies on. Found by manual trace while building a
    // targeted regression test for Bug 3 (see
    // cpp/tests/ingestion/two_thread_pipeline_test.cpp's
    // HighContentionWraparoundRegressionForBug3), not by a failing test —
    // no existing test exercised push/pop *behavior* at capacity 1, only
    // that construction succeeded. See cpp/ingestion/README.md.
    explicit SpscRingBuffer(std::size_t capacity)
        : capacity_(capacity), mask_(capacity - 1), cells_(std::make_unique<Cell[]>(capacity < 2 ? 1 : capacity)) {
        if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("SpscRingBuffer: capacity must be a power of two >= 2");
        }
        for (std::size_t i = 0; i < capacity_; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    // Producer-only. Never blocks. Returns true if the element was added
    // without dropping anything; false if the buffer was full and the
    // oldest unconsumed element was discarded to make room (see
    // dropped_count() for a running total).
    bool push(T item) {
        std::size_t pos = tail_.load(std::memory_order_relaxed);
        bool dropped = false;

        for (;;) {
            Cell& cell = cells_[pos & mask_];
            if (cell.sequence.load(std::memory_order_acquire) == pos) {
                break;  // vacated specifically for this logical index -- safe to write
            }
            // Not yet vacated for `pos` specifically: the buffer is full
            // from this position's perspective. A single reclaim isn't
            // guaranteed to free *this* slot (the consumer may have made
            // concurrent progress claiming a different index) -- loop and
            // re-check with a fresh sequence read after every attempt,
            // exactly as the Phase 3 design already knew to do for its own
            // head_-based check.
            T discarded;
            if (try_claim_front(discarded)) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                dropped = true;
            }
        }

        Cell& cell = cells_[pos & mask_];
        cell.data = std::move(item);
        cell.sequence.store(pos + 1, std::memory_order_release);
        tail_.store(pos + 1, std::memory_order_relaxed);
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
        std::size_t t = tail_.load(std::memory_order_relaxed);
        std::size_t h = head_.load(std::memory_order_relaxed);
        return t - h;
    }

    std::uint64_t dropped_count() const { return dropped_.load(std::memory_order_relaxed); }

private:
    struct Cell {
        std::atomic<std::size_t> sequence{0};
        T data{};
    };

    // Shared claim protocol for "take exclusive ownership of the front
    // element and remove it." Used by both pop() (the common case) and
    // push()'s drop-oldest reclaim (the rare, full-buffer case). The
    // per-cell sequence number is what makes this safe now — see the class
    // comment for the full story of why the Phase 3 predecessor of this
    // function (copy-before-CAS, no sequence numbers) wasn't sufficient.
    bool try_claim_front(T& out) {
        std::size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[pos & mask_];
            const std::size_t seq = cell.sequence.load(std::memory_order_acquire);
            if (seq == pos + 1) {
                // Producer's publish for this exact logical index is
                // visible. Attempt to win it.
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    out = std::move(cell.data);
                    // Frees this physical slot for the *next* wraparound
                    // (logical index pos + capacity_) -- release so a
                    // producer's subsequent acquire-load sees both this
                    // sequence update and the completed read of cell.data
                    // that happened-before it.
                    cell.sequence.store(pos + capacity_, std::memory_order_release);
                    return true;
                }
                // Lost the race for this index to a concurrent claimant
                // (pop() vs. push()'s reclaim). compare_exchange_weak
                // refreshed `pos` to the current head_ on failure; loop and
                // re-read *that* (different) cell's sequence. This loser
                // never touched cell.data -- only the atomic sequence field,
                // which is race-free by definition.
            } else if (seq == pos) {
                return false;  // nothing published for this index yet -- empty
            } else {
                // Stale local `pos`: some other claim already moved head_
                // past it. Reload and retry against the current value.
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<Cell[]> cells_;

    alignas(64) std::atomic<std::size_t> head_{0};  // consumer-owned index (cache-line separated
    alignas(64) std::atomic<std::size_t> tail_{0};  // producer-owned index  from head_ to avoid
                                                     // false sharing under sustained load)
    std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace tse::ingestion
