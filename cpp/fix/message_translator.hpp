#pragma once

#include <quickfix/fix42/ExecutionReport.h>
#include <quickfix/fix42/NewOrderSingle.h>
#include <quickfix/fix42/OrderCancelRequest.h>

#include "types.hpp"

namespace tse::fix {

// Order/Execution <-> real QuickFIX FIX42 message objects. Pure functions,
// no session dependency — these are what the round-trip tests exercise
// directly, and what fix_application.cpp calls when messages arrive on a
// live session.
//
// order.status must be kNew.
FIX42::NewOrderSingle to_new_order_single(const Order& order);
// order.status must be kCancelled.
FIX42::OrderCancelRequest to_order_cancel_request(const Order& order);
FIX42::ExecutionReport to_execution_report(const Execution& execution);

Order from_new_order_single(const FIX42::NewOrderSingle& message);
Order from_order_cancel_request(const FIX42::OrderCancelRequest& message);
Execution from_execution_report(const FIX42::ExecutionReport& message);

}  // namespace tse::fix
