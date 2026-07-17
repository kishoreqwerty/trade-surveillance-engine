#pragma once

#include <quickfix/Application.h>
#include <quickfix/MessageCracker.h>
#include <quickfix/Session.h>
#include <quickfix/fix42/ExecutionReport.h>
#include <quickfix/fix42/NewOrderSingle.h>
#include <quickfix/fix42/OrderCancelRequest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "types.hpp"

namespace tse::fix {

// The actual "session layer" piece: reacts to session lifecycle events and
// cracks incoming application messages into internal Order/Execution
// structs via message_translator. Sequence numbers, heartbeats, and
// gap/resend recovery are handled entirely by QuickFIX's own Session /
// SessionState state machine — this class doesn't reimplement any of that,
// it only observes it (resend_requests_seen()) for tests to verify it's
// actually happening.
//
// If message_translator throws FIX::FieldNotFound (a required field is
// missing) from inside onMessage, that propagates back through fromApp,
// which is exactly the exception QuickFIX's Application::fromApp contract
// expects callers to be able to throw — Session::next() catches it, sends a
// Reject, and keeps the session alive. No crash-prevention try/catch is
// needed here; that IS the crash-prevention mechanism.
// MessageCracker declares an onMessage() overload for every message type
// across every FIX version it supports, inherited from nine separate
// per-version base classes (FIX40::MessageCracker .. FIXT11::MessageCracker)
// — there's no single unified overload set a `using` declaration could pull
// in here. Overriding only the three FIX42 types we care about legitimately
// hides the rest, which is expected and harmless (we're not calling them),
// so -Woverloaded-virtual is suppressed for just this declaration rather
// than worked around.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverloaded-virtual"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
#endif

class SurveillanceFixApplication : public FIX::Application, public FIX::MessageCracker {
public:
    void onCreate(const FIX::SessionID&) override;
    void onLogon(const FIX::SessionID&) override;
    void onLogout(const FIX::SessionID&) override;
    void toAdmin(FIX::Message&, const FIX::SessionID&) override;
    void toApp(FIX::Message&, const FIX::SessionID&) EXCEPT(FIX::DoNotSend) override;
    void fromAdmin(const FIX::Message&, const FIX::SessionID&)
        EXCEPT(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::RejectLogon) override;
    void fromApp(const FIX::Message&, const FIX::SessionID&)
        EXCEPT(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue,
               FIX::UnsupportedMessageType) override;

    void onMessage(const FIX42::NewOrderSingle&, const FIX::SessionID&) override;
    void onMessage(const FIX42::OrderCancelRequest&, const FIX::SessionID&) override;
    void onMessage(const FIX42::ExecutionReport&, const FIX::SessionID&) override;

    bool wait_for_logon(std::chrono::milliseconds timeout);
    bool wait_for_logout(std::chrono::milliseconds timeout);
    bool wait_for_event_count(std::chrono::milliseconds timeout, size_t min_orders, size_t min_executions);

    std::vector<Order> received_orders() const;
    std::vector<Execution> received_executions() const;

    // How many ResendRequest(35=2) admin messages this side has seen —
    // direct evidence that QuickFIX's own gap-recovery machinery fired.
    int resend_requests_seen() const { return resend_requests_seen_; }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool logged_on_{false};
    bool logged_out_{false};
    std::vector<Order> received_orders_;
    std::vector<Execution> received_executions_;
    std::atomic<int> resend_requests_seen_{0};
};

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

}  // namespace tse::fix
