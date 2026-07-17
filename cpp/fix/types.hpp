#pragma once

#include <cstdint>
#include <string>

namespace tse::fix {

enum class Side { kBuy, kSell };

enum class OrderType { kLimit, kMarket };

// Book-mutating order lifecycle states this layer parses directly from the
// wire. Fills arrive as separate Execution records (ExecutionReport), not as
// an Order status — mirrors simulator/types.hpp's OrderStatus split and
// matches OrderBook::apply(Order) vs apply(Execution) in the architecture
// doc.
enum class OrderStatus { kNew, kCancelled, kReplaced };

const char* to_string(Side value);
const char* to_string(OrderType value);
const char* to_string(OrderStatus value);

Side side_from_string(const std::string& value);
OrderType order_type_from_string(const std::string& value);
OrderStatus order_status_from_string(const std::string& value);

// Live-mode data model — deliberately has no ground_truth_label field.
// Per CLAUDE.md's data-compliance rules, that field must never appear in a
// live-mode code path; here that's enforced structurally, not just by
// convention (compare simulator::Order/Execution, which carry the label and
// are evaluation/replay-mode only).
struct Order {
    std::string order_id;  // ClOrdID
    std::string account_id;
    std::string instrument_id;
    Side side{Side::kBuy};
    double price{0.0};
    int64_t qty{0};
    OrderType order_type{OrderType::kLimit};
    int64_t timestamp_ns{0};
    OrderStatus status{OrderStatus::kNew};
    std::string venue;

    // Only meaningful when status == kCancelled: the order_id of the order
    // being cancelled. Equal to order_id in the common case where this
    // layer doesn't track a separate cancel-request ID (mirrors Phase 1's
    // fix_writer simplification), but kept as its own field so a real
    // OrigClOrdID received off the wire is preserved exactly, not assumed.
    std::string orig_order_id;
};

struct Execution {
    std::string trade_id;  // ExecID
    std::string order_id;
    std::string account_id;
    std::string instrument_id;
    // Not in the architecture doc's Execution entity table, but FIX 4.2's
    // ExecutionReport declares Side(54) a required field on the wire — an
    // ExecutionReport can't be built or round-tripped without it, so it's
    // added here even though simulator::Execution (the pre-FIX, evaluation-
    // mode struct) doesn't carry it.
    Side side{Side::kBuy};
    double price{0.0};
    int64_t qty{0};
    int64_t timestamp_ns{0};
    std::string counterparty_account_id;
    std::string venue;
};

}  // namespace tse::fix
