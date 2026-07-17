#include "fix_session_test_fixture.hpp"

#include <unistd.h>

#include <atomic>

namespace tse::fix::testutil {

namespace {
// Ports 19870+ chosen arbitrarily to avoid common well-known ports.
//
// This atomic counter alone is NOT enough to avoid collisions: CTest's
// gtest_discover_tests runs every individual test as its own OS process
// (`fix_tests --gtest_filter=Suite.OneTest`), so this counter resets to 0
// for every single LiveSessionFixture test under `ctest` — it only spreads
// ports out for multiple fixture instances sharing one process (e.g.
// running the whole binary directly without a filter). Two back-to-back
// ctest-invoked processes would otherwise both claim port 19870, and the
// second can hang/time out binding a port still in the first's TIME_WAIT.
// PID-derived jitter fixes the cross-process case: consecutive OS
// processes get consecutive (or at least very-likely-distinct) PIDs, so
// salting with getpid() spreads separate ctest test processes across a
// wide port range while the atomic counter still spreads multiple
// fixtures within one process.
std::atomic<int> next_port_offset{0};

int allocate_port() {
    int pid_salt = (static_cast<int>(::getpid()) % 500) * 10;
    return 19870 + pid_salt + (next_port_offset++);
}
}  // namespace

void LiveSessionFixture::SetUp() {
    int port = allocate_port();

    acceptor_session_id_ = FIX::SessionID("FIX.4.2", "ACCEPTOR", "INITIATOR");
    initiator_session_id_ = FIX::SessionID("FIX.4.2", "INITIATOR", "ACCEPTOR");

    FIX::Dictionary common;
    common.setString("StartTime", "00:00:00");
    common.setString("EndTime", "00:00:00");
    common.setString("HeartBtInt", "5");
    common.setString("ReconnectInterval", "2");
    common.setString("DataDictionary", QUICKFIX_FIX42_SPEC_PATH);

    FIX::Dictionary acceptor_dict = common;
    acceptor_dict.setString("ConnectionType", "acceptor");
    acceptor_dict.setInt("SocketAcceptPort", port);

    FIX::Dictionary initiator_dict = common;
    initiator_dict.setString("ConnectionType", "initiator");
    initiator_dict.setString("SocketConnectHost", "127.0.0.1");
    initiator_dict.setInt("SocketConnectPort", port);

    acceptor_settings_ = std::make_unique<FIX::SessionSettings>();
    acceptor_settings_->set(common);
    acceptor_settings_->set(acceptor_session_id_, acceptor_dict);

    initiator_settings_ = std::make_unique<FIX::SessionSettings>();
    initiator_settings_->set(common);
    initiator_settings_->set(initiator_session_id_, initiator_dict);

    acceptor_store_factory_ = std::make_unique<FIX::MemoryStoreFactory>();
    initiator_store_factory_ = std::make_unique<FIX::MemoryStoreFactory>();
    // (incoming, outgoing, event) all false — keep test output clean; this
    // is still a real LogFactory, just a quiet one.
    acceptor_log_factory_ = std::make_unique<FIX::ScreenLogFactory>(false, false, false);
    initiator_log_factory_ = std::make_unique<FIX::ScreenLogFactory>(false, false, false);

    acceptor_ = std::make_unique<FIX::SocketAcceptor>(acceptor_app_, *acceptor_store_factory_,
                                                       *acceptor_settings_, *acceptor_log_factory_);
    acceptor_->start();

    initiator_ = std::make_unique<FIX::SocketInitiator>(initiator_app_, *initiator_store_factory_,
                                                         *initiator_settings_, *initiator_log_factory_);
    initiator_->start();
}

void LiveSessionFixture::TearDown() {
    if (initiator_) initiator_->stop();
    if (acceptor_) acceptor_->stop();
    initiator_.reset();
    acceptor_.reset();
}

bool LiveSessionFixture::logon(std::chrono::milliseconds timeout) {
    bool acceptor_ok = acceptor_app_.wait_for_logon(timeout);
    bool initiator_ok = initiator_app_.wait_for_logon(timeout);
    return acceptor_ok && initiator_ok;
}

}  // namespace tse::fix::testutil
