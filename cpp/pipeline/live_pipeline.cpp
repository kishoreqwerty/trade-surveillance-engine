#include "live_pipeline.hpp"

#include <chrono>
#include <stdexcept>
#include <variant>

namespace tse::pipeline {

using tse::detectors::Alert;
using tse::detectors::DetectorEvent;
using tse::orderbook::OrderBook;

namespace {
const std::string& instrument_id_of(const DetectorEvent& event) {
    return std::visit([](const auto& concrete) -> const std::string& { return concrete.instrument_id; }, event);
}

int64_t duration_ns(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
}
}  // namespace

LivePipeline::LivePipeline(std::vector<std::unique_ptr<tse::detectors::IDetector>> detectors,
                            tse::detectors::AccountRegistry accounts)
    : detectors_(std::move(detectors)), accounts_(std::move(accounts)) {}

const OrderBook* LivePipeline::book_for(const std::string& instrument_id) const {
    auto it = books_.find(instrument_id);
    return it == books_.end() ? nullptr : &it->second;
}

ProcessResult LivePipeline::process(const DetectorEvent& event) {
    const std::string& instrument_id = instrument_id_of(event);
    // Constructs the OrderBook in-place if this is the first event for this
    // instrument -- unordered_map's node-based storage means this never
    // needs OrderBook to be movable (see order_book.hpp: copy is deleted
    // and there's no user-declared move either).
    OrderBook& book = books_.try_emplace(instrument_id, instrument_id).first->second;

    ProcessResult result;

    const auto book_start = std::chrono::steady_clock::now();
    try {
        std::visit([&book](const auto& concrete) { book.apply(concrete); }, event);
    } catch (const std::invalid_argument&) {
        // See class comment: a drop-oldest-induced inconsistency, not a bug
        // to crash the consumer thread over.
        ++inconsistent_events_skipped_;
        result.skipped_inconsistent = true;
        return result;
    }
    const auto book_end = std::chrono::steady_clock::now();
    result.book_apply_ns = duration_ns(book_start, book_end);

    const auto detectors_start = std::chrono::steady_clock::now();
    for (const auto& detector : detectors_) {
        std::vector<Alert> found = detector->evaluate(book, event, accounts_);
        result.alerts.insert(result.alerts.end(), std::make_move_iterator(found.begin()),
                              std::make_move_iterator(found.end()));
    }
    const auto detectors_end = std::chrono::steady_clock::now();
    result.detectors_ns = duration_ns(detectors_start, detectors_end);

    return result;
}

}  // namespace tse::pipeline
