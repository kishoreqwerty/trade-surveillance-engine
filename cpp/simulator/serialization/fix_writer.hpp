#pragma once

#include <string>
#include <vector>

#include "types.hpp"

namespace tse::simulator {

// Builds well-formed FIX 4.2 tag=value messages (correct BodyLength and
// CheckSum) for NewOrderSingle, OrderCancelRequest, and ExecutionReport —
// the three message types Phase 2's QuickFIX parser consumes. This is body
// serialization only: session-layer concerns (logon, heartbeats, sequence
// recovery) are Phase 2's job, not this generator's.
//
// None of these builders read Order::ground_truth_label or
// Execution::ground_truth_label — FIX text output is structurally
// incapable of leaking it, which is what makes it the live-mode path.
class FixMessageBuilder {
public:
    explicit FixMessageBuilder(std::string sender_comp_id = "TSE-SIM",
                                std::string target_comp_id = "TSE-EXCH");

    // Order.status must be kNew.
    std::string build_new_order_single(const Order& order);
    // Order.status must be kCancelled; order_id is used as both ClOrdID and
    // OrigClOrdID (this generator doesn't track a separate cancel-request ID).
    std::string build_order_cancel_request(const Order& cancel_order);
    std::string build_execution_report(const Execution& execution);

private:
    std::string sender_comp_id_;
    std::string target_comp_id_;
    int64_t seq_num_{1};

    std::string assemble(const std::string& msg_type, const std::string& body_fields,
                          int64_t timestamp_ns);
};

// Merges orders and executions in timestamp order and renders each as its
// corresponding FIX message (kNew/kCancelled orders, all executions).
// kReplaced orders are skipped — no injector in this phase emits them yet.
std::vector<std::string> to_fix_messages(const std::vector<Order>& orders,
                                          const std::vector<Execution>& executions);

}  // namespace tse::simulator
