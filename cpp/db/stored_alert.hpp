#pragma once

#include <cstdint>

#include "alert.hpp"

namespace tse::db {

// A persisted Alert plus the identity the store assigns at insert time.
// alert_id is exactly the field detectors::Alert deliberately omits (see
// alert.hpp: "assigned at persistence time -- Phase 8's concern, not a
// detector's").
struct StoredAlert {
    int64_t alert_id{0};
    tse::detectors::Alert alert;
};

}  // namespace tse::db
