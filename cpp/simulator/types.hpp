#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tse::simulator {

enum class AssetClass { kEquity, kFx, kFixedIncome };

enum class Side { kBuy, kSell };

enum class OrderType { kLimit, kMarket };

// Book-mutating order lifecycle states this generator emits directly.
// Fills are represented purely via Execution records, not as an Order
// status, matching OrderBook::apply(Order) vs apply(Execution) in the
// architecture doc (add/cancel/replace go through Order, execute through
// Execution).
enum class OrderStatus { kNew, kCancelled, kReplaced };

enum class EntityType { kClient, kProprietary };

enum class AbusePattern {
    kBaseline,  // not part of any injected abuse scenario
    kWashTrade,
    kSpoofingLayering,
    kMarkingTheClose,
    kFrontRunning,
};

const char* to_string(AssetClass value);
const char* to_string(Side value);
const char* to_string(OrderType value);
const char* to_string(OrderStatus value);
const char* to_string(EntityType value);
const char* to_string(AbusePattern value);

// Inverse of the to_string functions above — throws std::invalid_argument on
// an unrecognized value. Used by the JSON/CSV parsers to round-trip
// serialized events back into structs.
Side side_from_string(const std::string& value);
OrderType order_type_from_string(const std::string& value);
OrderStatus order_status_from_string(const std::string& value);
AbusePattern abuse_pattern_from_string(const std::string& value);

struct Instrument {
    std::string instrument_id;
    std::string symbol;
    AssetClass asset_class{AssetClass::kEquity};
    double tick_size{0.01};
    int64_t avg_daily_volume{0};
    int64_t session_close_ns{0};  // absolute epoch ns this synthetic session closes at
};

struct Account {
    std::string account_id;
    std::string beneficial_owner_id;
    EntityType entity_type{EntityType::kClient};
    std::vector<std::string> linked_account_ids;
};

// Evaluation-only evidence tying a generated event back to the injected
// scenario that produced it. Every Order/Execution carries one — baseline
// (non-abusive) events carry the kBaseline sentinel. Per CLAUDE.md's data
// rules, this must never appear in a live-mode code path; the FIX
// serialization in serialization/fix_writer.hpp never reads this field.
struct GroundTruthLabel {
    AbusePattern pattern{AbusePattern::kBaseline};
    std::string scenario_id;  // empty for baseline events
    double severity{0.0};
};

inline bool is_abuse(const GroundTruthLabel& label) {
    return label.pattern != AbusePattern::kBaseline;
}

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
    GroundTruthLabel ground_truth_label;
};

struct Execution {
    std::string trade_id;  // ExecID
    std::string order_id;
    std::string account_id;
    std::string instrument_id;
    double price{0.0};
    int64_t qty{0};
    int64_t timestamp_ns{0};
    std::string counterparty_account_id;
    std::string venue;
    GroundTruthLabel ground_truth_label;
};

// Returns a copy with ground_truth_label reset to the baseline sentinel —
// used to prove live-mode output is indistinguishable regardless of origin.
Order strip_label(Order order);
Execution strip_label(Execution execution);

}  // namespace tse::simulator
