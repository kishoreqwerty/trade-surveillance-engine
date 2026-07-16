#include "types.hpp"

#include <stdexcept>

namespace tse::simulator {

const char* to_string(AssetClass value) {
    switch (value) {
        case AssetClass::kEquity: return "Equity";
        case AssetClass::kFx: return "FX";
        case AssetClass::kFixedIncome: return "FixedIncome";
    }
    return "Unknown";
}

const char* to_string(Side value) {
    switch (value) {
        case Side::kBuy: return "Buy";
        case Side::kSell: return "Sell";
    }
    return "Unknown";
}

const char* to_string(OrderType value) {
    switch (value) {
        case OrderType::kLimit: return "Limit";
        case OrderType::kMarket: return "Market";
    }
    return "Unknown";
}

const char* to_string(OrderStatus value) {
    switch (value) {
        case OrderStatus::kNew: return "New";
        case OrderStatus::kCancelled: return "Cancelled";
        case OrderStatus::kReplaced: return "Replaced";
    }
    return "Unknown";
}

const char* to_string(EntityType value) {
    switch (value) {
        case EntityType::kClient: return "Client";
        case EntityType::kProprietary: return "Proprietary";
    }
    return "Unknown";
}

const char* to_string(AbusePattern value) {
    switch (value) {
        case AbusePattern::kBaseline: return "Baseline";
        case AbusePattern::kWashTrade: return "WashTrade";
        case AbusePattern::kSpoofingLayering: return "SpoofingLayering";
        case AbusePattern::kMarkingTheClose: return "MarkingTheClose";
        case AbusePattern::kFrontRunning: return "FrontRunning";
    }
    return "Unknown";
}

Side side_from_string(const std::string& value) {
    if (value == "Buy") return Side::kBuy;
    if (value == "Sell") return Side::kSell;
    throw std::invalid_argument("side_from_string: unrecognized value '" + value + "'");
}

OrderType order_type_from_string(const std::string& value) {
    if (value == "Limit") return OrderType::kLimit;
    if (value == "Market") return OrderType::kMarket;
    throw std::invalid_argument("order_type_from_string: unrecognized value '" + value + "'");
}

OrderStatus order_status_from_string(const std::string& value) {
    if (value == "New") return OrderStatus::kNew;
    if (value == "Cancelled") return OrderStatus::kCancelled;
    if (value == "Replaced") return OrderStatus::kReplaced;
    throw std::invalid_argument("order_status_from_string: unrecognized value '" + value + "'");
}

AbusePattern abuse_pattern_from_string(const std::string& value) {
    if (value == "Baseline") return AbusePattern::kBaseline;
    if (value == "WashTrade") return AbusePattern::kWashTrade;
    if (value == "SpoofingLayering") return AbusePattern::kSpoofingLayering;
    if (value == "MarkingTheClose") return AbusePattern::kMarkingTheClose;
    if (value == "FrontRunning") return AbusePattern::kFrontRunning;
    throw std::invalid_argument("abuse_pattern_from_string: unrecognized value '" + value + "'");
}

Order strip_label(Order order) {
    order.ground_truth_label = GroundTruthLabel{};
    return order;
}

Execution strip_label(Execution execution) {
    execution.ground_truth_label = GroundTruthLabel{};
    return execution;
}

}  // namespace tse::simulator
