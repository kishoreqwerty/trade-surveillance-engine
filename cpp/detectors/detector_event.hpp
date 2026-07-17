#pragma once

#include <variant>

#include "types.hpp"  // tse::fix::Order, tse::fix::Execution

namespace tse::detectors {

// Everything a detector might need to react to. Deliberately a local alias
// rather than reusing tse::ingestion::IngestionEvent (the same underlying
// shape, std::variant<Order, Execution>) — the architecture doc's
// dependency table has detectors/ depending only on orderbook/, not
// ingestion/, and pulling in ingestion/ just for this one alias would add a
// real (if harmless) module coupling the doc doesn't call for. See
// i_detector.hpp for why evaluate() needs both event types, not just Order.
using DetectorEvent = std::variant<tse::fix::Order, tse::fix::Execution>;

}  // namespace tse::detectors
