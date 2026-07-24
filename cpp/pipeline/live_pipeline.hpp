#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "account_registry.hpp"
#include "detector_event.hpp"
#include "i_detector.hpp"
#include "order_book.hpp"

namespace tse::pipeline {

struct ProcessResult {
    std::vector<tse::detectors::Alert> alerts;
    int64_t book_apply_ns{0};
    int64_t detectors_ns{0};
    // True iff this event was skipped (see class comment) rather than
    // genuinely producing zero alerts -- book_apply_ns/detectors_ns are
    // both 0 when this is true.
    bool skipped_inconsistent{false};
};

// The architecture doc's live (and, later, replay) data flow condensed
// into one object: "orderbook/ applies the update, produces new book
// state -> detectors/ each IDetector.evaluate() runs against the updated
// book" (P2_trade_surveillance_engine_architecture.md §4), plus the
// per-instrument book routing that was explicitly deferred out of Phase 4
// (order_book.hpp: "routing events to the right per-instrument OrderBook
// ... is Phase 6's live-pipeline wiring concern, not the book engine's").
//
// Not thread-safe, by design, not by oversight: exactly one consumer
// thread ever calls process() (the SPSC ring buffer upstream guarantees a
// single consumer — see cpp/ingestion/spsc_ring_buffer.hpp), so no
// internal synchronization is needed, mirroring OrderBook's own
// single-threaded-by-design stance from Phase 4.
//
// Handles a real consequence of Phase 3's drop-oldest backpressure policy
// that only becomes reachable once ingestion/, orderbook/, and detectors/
// are actually wired together under load: dropping an event from the ring
// buffer can produce a *logically inconsistent* sequence reaching
// OrderBook — e.g. an Execution or Replace referencing an order_id whose
// New was the event that got dropped. OrderBook::apply() is deliberately
// designed (Phase 4) to throw std::invalid_argument for exactly this
// class of "genuine invariant violation, not a normal race" — a
// reasonable stance in Phase 4's single-threaded, hand-constructed-
// sequence world, where such a thing could only mean a test bug. Under
// real sustained load with drop-oldest active, it can also mean "the ring
// buffer legitimately dropped the prerequisite event," which is an
// accepted, already-documented consequence of that policy (see
// cpp/ingestion/README.md), not a defect to crash the consumer thread
// over. process() therefore catches std::invalid_argument specifically
// around the book-apply step (not a blanket catch — this is the one
// documented exception type OrderBook::apply() throws), counts it via
// inconsistent_events_skipped(), and skips detector evaluation for that
// event rather than propagating.
class LivePipeline {
public:
    LivePipeline(std::vector<std::unique_ptr<tse::detectors::IDetector>> detectors,
                 tse::detectors::AccountRegistry accounts);

    ProcessResult process(const tse::detectors::DetectorEvent& event);

    // nullptr if this instrument has never had an event applied.
    const tse::orderbook::OrderBook* book_for(const std::string& instrument_id) const;

    uint64_t inconsistent_events_skipped() const { return inconsistent_events_skipped_; }

    // Fixed at construction, never mutated after -- safe to read from any
    // thread without the caller taking a lock (there is no writer to race
    // against). api/'s status endpoint uses this for its "detectors
    // active" tile.
    size_t detector_count() const { return detectors_.size(); }

private:
    std::vector<std::unique_ptr<tse::detectors::IDetector>> detectors_;
    tse::detectors::AccountRegistry accounts_;
    std::unordered_map<std::string, tse::orderbook::OrderBook> books_;
    uint64_t inconsistent_events_skipped_{0};
};

}  // namespace tse::pipeline
