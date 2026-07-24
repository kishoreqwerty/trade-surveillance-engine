#include "json_encode.hpp"

namespace tse::api {

crow::json::wvalue encode_alert(const tse::db::StoredAlert& stored) {
    crow::json::wvalue out;
    out["alert_id"] = stored.alert_id;
    out["status"] = stored.status;
    out["detector_name"] = stored.alert.detector_name;
    out["score"] = stored.alert.score;
    out["instrument_id"] = stored.alert.instrument_id;

    std::vector<crow::json::wvalue> account_ids;
    account_ids.reserve(stored.alert.account_ids.size());
    for (const auto& id : stored.alert.account_ids) account_ids.push_back(id);
    out["account_ids"] = crow::json::wvalue(std::move(account_ids));

    std::vector<crow::json::wvalue> order_ids;
    order_ids.reserve(stored.alert.order_ids.size());
    for (const auto& id : stored.alert.order_ids) order_ids.push_back(id);
    out["order_ids"] = crow::json::wvalue(std::move(order_ids));

    out["window_start_ns"] = stored.alert.window_start_ns;
    out["window_end_ns"] = stored.alert.window_end_ns;
    out["evidence"] = stored.alert.evidence;
    out["model_version"] = stored.alert.model_version.has_value() ? crow::json::wvalue(*stored.alert.model_version)
                                                                    : crow::json::wvalue(nullptr);
    out["book_snapshot_sequence"] = stored.alert.book_snapshot_sequence.has_value()
                                         ? crow::json::wvalue(*stored.alert.book_snapshot_sequence)
                                         : crow::json::wvalue(nullptr);
    return out;
}

crow::json::wvalue encode_alerts(const std::vector<tse::db::StoredAlert>& alerts) {
    std::vector<crow::json::wvalue> items;
    items.reserve(alerts.size());
    for (const auto& stored : alerts) items.push_back(encode_alert(stored));
    crow::json::wvalue out;
    out["alerts"] = crow::json::wvalue(std::move(items));
    return out;
}

crow::json::wvalue encode_alerts(const std::vector<tse::db::StoredAlert>& alerts, int64_t total_count) {
    crow::json::wvalue out = encode_alerts(alerts);
    out["total_count"] = total_count;
    return out;
}

namespace {
crow::json::wvalue encode_price_level(const tse::orderbook::PriceLevel& level) {
    crow::json::wvalue out;
    out["price"] = level.price;
    out["total_qty"] = level.total_qty;
    std::vector<crow::json::wvalue> orders;
    orders.reserve(level.orders.size());
    for (const auto& resting : level.orders) {
        crow::json::wvalue order;
        order["order_id"] = resting.order_id;
        order["account_id"] = resting.account_id;
        order["qty"] = resting.qty;
        orders.push_back(std::move(order));
    }
    out["orders"] = crow::json::wvalue(std::move(orders));
    return out;
}

std::vector<crow::json::wvalue> encode_price_levels(const std::vector<tse::orderbook::PriceLevel>& levels) {
    std::vector<crow::json::wvalue> out;
    out.reserve(levels.size());
    for (const auto& level : levels) out.push_back(encode_price_level(level));
    return out;
}
}  // namespace

crow::json::wvalue encode_depth_snapshot(const tse::orderbook::DepthSnapshot& snapshot) {
    crow::json::wvalue out;
    out["instrument_id"] = snapshot.instrument_id;
    out["sequence"] = snapshot.sequence;
    out["last_event_timestamp_ns"] = snapshot.last_event_timestamp_ns;
    out["bids"] = crow::json::wvalue(encode_price_levels(snapshot.bids));
    out["asks"] = crow::json::wvalue(encode_price_levels(snapshot.asks));
    return out;
}

crow::json::wvalue encode_book_events(const std::vector<tse::api::BookEvent>& events) {
    std::vector<crow::json::wvalue> items;
    items.reserve(events.size());
    for (const auto& event : events) {
        crow::json::wvalue item;
        item["timestamp_ns"] = event.timestamp_ns;
        item["instrument_id"] = event.instrument_id;
        item["msg_type"] = event.msg_type;
        item["side"] = event.side;
        item["price"] = event.price;
        item["qty"] = event.qty;
        item["order_id"] = event.order_id;
        item["account_id"] = event.account_id;
        items.push_back(std::move(item));
    }
    crow::json::wvalue out;
    out["events"] = crow::json::wvalue(std::move(items));
    return out;
}

}  // namespace tse::api
