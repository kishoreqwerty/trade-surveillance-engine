#pragma once

#include <quickfix/Dictionary.h>
#include <quickfix/Log.h>
#include <quickfix/MessageStore.h>
#include <quickfix/Session.h>
#include <quickfix/SessionID.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketAcceptor.h>
#include <quickfix/SocketInitiator.h>

#include <chrono>
#include <memory>

#include <gtest/gtest.h>

#include "fix_application.hpp"

namespace tse::fix::testutil {

// Brings up a real FIX 4.2 acceptor+initiator pair over loopback TCP —
// genuine Logon handshake, genuine Session/SessionState sequence-number and
// heartbeat bookkeeping, genuine socket transport. Test-only bootstrap, not
// part of the production tse_fix library. Each instance gets its own port
// (a static counter) so sequential tests in the same binary don't collide
// on a port still in TIME_WAIT from the previous test's teardown.
class LiveSessionFixture : public ::testing::Test {
protected:
    void SetUp() override;
    void TearDown() override;

    // Waits for both sides to report onLogon(). False on timeout.
    bool logon(std::chrono::milliseconds timeout = std::chrono::seconds(5));

    SurveillanceFixApplication acceptor_app_;
    SurveillanceFixApplication initiator_app_;
    FIX::SessionID acceptor_session_id_;
    FIX::SessionID initiator_session_id_;

private:
    std::unique_ptr<FIX::SessionSettings> acceptor_settings_;
    std::unique_ptr<FIX::SessionSettings> initiator_settings_;
    std::unique_ptr<FIX::MemoryStoreFactory> acceptor_store_factory_;
    std::unique_ptr<FIX::MemoryStoreFactory> initiator_store_factory_;
    std::unique_ptr<FIX::ScreenLogFactory> acceptor_log_factory_;
    std::unique_ptr<FIX::ScreenLogFactory> initiator_log_factory_;
    std::unique_ptr<FIX::SocketAcceptor> acceptor_;
    std::unique_ptr<FIX::SocketInitiator> initiator_;
};

}  // namespace tse::fix::testutil
