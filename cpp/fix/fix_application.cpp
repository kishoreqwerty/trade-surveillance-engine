#include "fix_application.hpp"

#include "message_translator.hpp"

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
// Forces the linker to pull tsan_suppressions.o out of libtse_fix.a. Static
// archives only link in object files that resolve some other object's
// undefined symbol — nothing in this codebase *calls*
// __tsan_default_suppressions() (only TSan's runtime looks it up, by name,
// at startup), so without this the linker silently drops it and the
// suppressions compiled into tsan_suppressions.cpp never take effect. This
// translation unit is unconditionally part of tse_fix, so the reference
// below is what actually keeps that object file in the final binary.
extern "C" const char* __tsan_default_suppressions();
namespace {
struct ForceTsanSuppressionsLink {
    ForceTsanSuppressionsLink() { (void)__tsan_default_suppressions(); }
} force_tsan_suppressions_link;
}  // namespace
#endif
#endif

namespace tse::fix {

void SurveillanceFixApplication::onCreate(const FIX::SessionID&) {}

void SurveillanceFixApplication::onLogon(const FIX::SessionID&) {
    std::lock_guard<std::mutex> lock(mutex_);
    logged_on_ = true;
    cv_.notify_all();
}

void SurveillanceFixApplication::onLogout(const FIX::SessionID&) {
    std::lock_guard<std::mutex> lock(mutex_);
    logged_out_ = true;
    cv_.notify_all();
}

void SurveillanceFixApplication::toAdmin(FIX::Message&, const FIX::SessionID&) {}

void SurveillanceFixApplication::toApp(FIX::Message&, const FIX::SessionID&) {}

void SurveillanceFixApplication::fromAdmin(const FIX::Message& message, const FIX::SessionID&) {
    // MsgType(35) is always present on a message that made it this far —
    // read it generically rather than via a typed field since we only need
    // to recognize ResendRequest, not fully crack admin messages.
    if (message.getHeader().getField(35) == "2") {
        ++resend_requests_seen_;
    }
}

void SurveillanceFixApplication::fromApp(const FIX::Message& message, const FIX::SessionID& sessionID) {
    crack(message, sessionID);
}

void SurveillanceFixApplication::onMessage(const FIX42::NewOrderSingle& message, const FIX::SessionID&) {
    Order order = from_new_order_single(message);
    std::lock_guard<std::mutex> lock(mutex_);
    received_orders_.push_back(std::move(order));
    cv_.notify_all();
}

void SurveillanceFixApplication::onMessage(const FIX42::OrderCancelRequest& message, const FIX::SessionID&) {
    Order order = from_order_cancel_request(message);
    std::lock_guard<std::mutex> lock(mutex_);
    received_orders_.push_back(std::move(order));
    cv_.notify_all();
}

void SurveillanceFixApplication::onMessage(const FIX42::ExecutionReport& message, const FIX::SessionID&) {
    Execution execution = from_execution_report(message);
    std::lock_guard<std::mutex> lock(mutex_);
    received_executions_.push_back(std::move(execution));
    cv_.notify_all();
}

bool SurveillanceFixApplication::wait_for_logon(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return logged_on_; });
}

bool SurveillanceFixApplication::wait_for_logout(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return logged_out_; });
}

bool SurveillanceFixApplication::wait_for_event_count(std::chrono::milliseconds timeout, size_t min_orders,
                                                       size_t min_executions) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, min_orders, min_executions] {
        return received_orders_.size() >= min_orders && received_executions_.size() >= min_executions;
    });
}

std::vector<Order> SurveillanceFixApplication::received_orders() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_orders_;
}

std::vector<Execution> SurveillanceFixApplication::received_executions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_executions_;
}

}  // namespace tse::fix
