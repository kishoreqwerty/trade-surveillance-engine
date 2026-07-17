#include "types.hpp"

#include <stdexcept>

namespace tse::fix {

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

}  // namespace tse::fix
