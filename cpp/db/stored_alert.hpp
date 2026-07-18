#pragma once

#include <cstdint>
#include <string>

#include "alert.hpp"

namespace tse::db {

// A persisted Alert plus the identity and case-management state the store
// itself owns. alert_id and status are exactly the two fields
// detectors::Alert deliberately omits (see alert.hpp: "assigned at
// persistence time" / "case-management state -- Phase 9's concern, not a
// detector's"). status starts "OPEN" (schema.sql's column default) and only
// ever changes via AlertStore::update_alert_status().
struct StoredAlert {
    int64_t alert_id{0};
    tse::detectors::Alert alert;
    std::string status{"OPEN"};
};

}  // namespace tse::db
