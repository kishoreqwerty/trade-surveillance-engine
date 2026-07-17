#include "live_pipeline.hpp"

#include <gtest/gtest.h>

#include "wash_trade_detector.hpp"

using tse::detectors::AccountRegistry;
using tse::detectors::Alert;
using tse::detectors::DetectorEvent;
using tse::detectors::Entity;
using tse::detectors::IDetector;
using tse::detectors::WashTradeDetector;
using tse::fix::Execution;
using tse::fix::Order;
using tse::fix::OrderStatus;
using tse::fix::OrderType;
using tse::fix::Side;
using tse::pipeline::LivePipeline;
using tse::pipeline::ProcessResult;

namespace {

// Records how many times evaluate() was called (and for which instrument),
// without implementing any real detection logic -- used to prove
// LivePipeline actually dispatches every event to every registered
// detector, independent of whether any real detector's own pattern logic
// fires. Phase 5 already exhaustively tests each detector's own logic in
// isolation; this file's job is to test the *wiring*, not re-test that.
class CountingDetector : public IDetector {
public:
    std::vector<Alert> evaluate(const tse::orderbook::OrderBook&, const DetectorEvent& incoming,
                                 const AccountRegistry&) override {
        ++call_count_;
        last_instrument_id_ = std::visit([](const auto& e) { return e.instrument_id; }, incoming);
        return {};
    }
    std::string name() const override { return "CountingDetector"; }

    int call_count_ = 0;
    std::string last_instrument_id_;
};

Order make_new(const std::string& id, const std::string& account, const std::string& instrument, Side side,
               double price, int64_t qty, int64_t ts) {
    Order order;
    order.order_id = id;
    order.orig_order_id = id;
    order.account_id = account;
    order.instrument_id = instrument;
    order.side = side;
    order.price = price;
    order.qty = qty;
    order.order_type = OrderType::kLimit;
    order.timestamp_ns = ts;
    order.status = OrderStatus::kNew;
    order.venue = "SIM";
    return order;
}

Execution make_execution(const std::string& order_id, const std::string& account, const std::string& counterparty,
                          const std::string& instrument, int64_t qty, int64_t ts) {
    Execution execution;
    execution.trade_id = "EXE-" + order_id;
    execution.order_id = order_id;
    execution.account_id = account;
    execution.instrument_id = instrument;
    execution.side = Side::kBuy;
    execution.price = 100.00;
    execution.qty = qty;
    execution.timestamp_ns = ts;
    execution.counterparty_account_id = counterparty;
    execution.venue = "SIM";
    return execution;
}

std::vector<std::unique_ptr<IDetector>> single_counting_detector(CountingDetector** out) {
    auto detector = std::make_unique<CountingDetector>();
    *out = detector.get();
    std::vector<std::unique_ptr<IDetector>> detectors;
    detectors.push_back(std::move(detector));
    return detectors;
}

}  // namespace

TEST(LivePipeline, RoutesEventsToPerInstrumentOrderBooksIndependently) {
    CountingDetector* counting = nullptr;
    LivePipeline pipeline(single_counting_detector(&counting), AccountRegistry{});

    pipeline.process(DetectorEvent{make_new("A1", "ACC-1", "ACME", Side::kBuy, 100.00, 500, 1000)});
    pipeline.process(DetectorEvent{make_new("B1", "ACC-1", "BETA", Side::kSell, 50.00, 300, 1001)});

    const auto* acme_book = pipeline.book_for("ACME");
    const auto* beta_book = pipeline.book_for("BETA");
    ASSERT_NE(acme_book, nullptr);
    ASSERT_NE(beta_book, nullptr);
    EXPECT_EQ(acme_book->qty_at_price(Side::kBuy, 100.00), 500);
    EXPECT_EQ(acme_book->qty_at_price(Side::kSell, 50.00), 0);  // no cross-contamination
    EXPECT_EQ(beta_book->qty_at_price(Side::kSell, 50.00), 300);
    EXPECT_EQ(beta_book->qty_at_price(Side::kBuy, 100.00), 0);
}

TEST(LivePipeline, BookForReturnsNullptrForNeverSeenInstrument) {
    CountingDetector* counting = nullptr;
    LivePipeline pipeline(single_counting_detector(&counting), AccountRegistry{});
    EXPECT_EQ(pipeline.book_for("NEVER_SEEN"), nullptr);
}

TEST(LivePipeline, EveryRegisteredDetectorIsInvokedForEveryEvent) {
    std::vector<std::unique_ptr<IDetector>> detectors;
    auto d1 = std::make_unique<CountingDetector>();
    auto d2 = std::make_unique<CountingDetector>();
    auto d3 = std::make_unique<CountingDetector>();
    CountingDetector* p1 = d1.get();
    CountingDetector* p2 = d2.get();
    CountingDetector* p3 = d3.get();
    detectors.push_back(std::move(d1));
    detectors.push_back(std::move(d2));
    detectors.push_back(std::move(d3));

    LivePipeline pipeline(std::move(detectors), AccountRegistry{});
    pipeline.process(DetectorEvent{make_new("O1", "ACC-1", "ACME", Side::kBuy, 100.00, 500, 1000)});
    pipeline.process(DetectorEvent{make_new("O2", "ACC-1", "ACME", Side::kBuy, 99.00, 200, 1001)});

    EXPECT_EQ(p1->call_count_, 2);
    EXPECT_EQ(p2->call_count_, 2);
    EXPECT_EQ(p3->call_count_, 2);
    EXPECT_EQ(p1->last_instrument_id_, "ACME");
}

// The actual wiring proof this file exists for: a real detector
// (WashTradeDetector, already exhaustively tested in isolation in Phase 5)
// fires when driven through the full LivePipeline -- OrderBook and
// AccountRegistry genuinely flow through, not just placeholder arguments.
TEST(LivePipeline, RealDetectorFiresWhenDrivenThroughTheFullPipeline) {
    AccountRegistry accounts;
    accounts.add(Entity{"ACC-1", "OWNER-A", "client", {}});
    accounts.add(Entity{"ACC-2", "OWNER-A", "client", {}});

    std::vector<std::unique_ptr<IDetector>> detectors;
    detectors.push_back(std::make_unique<WashTradeDetector>());
    LivePipeline pipeline(std::move(detectors), std::move(accounts));

    // A resting New must exist before an Execution can reference it — the
    // Execution below fully fills it, exactly as a live pipeline would
    // actually see: New first, then the ExecutionReport for the fill.
    pipeline.process(DetectorEvent{make_new("O1", "ACC-1", "ACME", Side::kBuy, 100.00, 500, 999)});
    ProcessResult result =
        pipeline.process(DetectorEvent{make_execution("O1", "ACC-1", "ACC-2", "ACME", 500, 1000)});

    ASSERT_EQ(result.alerts.size(), 1u);
    EXPECT_EQ(result.alerts[0].detector_name, "WashTradeDetector");
    EXPECT_FALSE(result.skipped_inconsistent);
    EXPECT_GE(result.book_apply_ns, 0);
    EXPECT_GE(result.detectors_ns, 0);
}

// The real consequence of Phase 3's drop-oldest policy that only becomes
// reachable once the pieces are wired together: an Execution referencing
// an order_id whose New was never seen (as if the ring buffer had dropped
// it) must not crash the pipeline -- OrderBook::apply() throws
// std::invalid_argument for this by design (Phase 4), and LivePipeline
// must catch specifically that, not let it propagate.
TEST(LivePipeline, ExecutionForNeverSeenOrderIsSkippedNotThrown) {
    CountingDetector* counting = nullptr;
    LivePipeline pipeline(single_counting_detector(&counting), AccountRegistry{});

    ProcessResult result;
    EXPECT_NO_THROW(
        result = pipeline.process(DetectorEvent{make_execution("NEVER_SEEN", "ACC-1", "", "ACME", 100, 1000)}));

    EXPECT_TRUE(result.skipped_inconsistent);
    EXPECT_TRUE(result.alerts.empty());
    EXPECT_EQ(result.book_apply_ns, 0);
    EXPECT_EQ(result.detectors_ns, 0);
    EXPECT_EQ(pipeline.inconsistent_events_skipped(), 1u);
    // Detectors must not have been consulted for a skipped event -- there's
    // no valid post-update book state to hand them.
    EXPECT_EQ(counting->call_count_, 0);
}

TEST(LivePipeline, SkippedEventsAreCountedCumulatively) {
    CountingDetector* counting = nullptr;
    LivePipeline pipeline(single_counting_detector(&counting), AccountRegistry{});

    pipeline.process(DetectorEvent{make_execution("NEVER_SEEN_1", "ACC-1", "", "ACME", 100, 1000)});
    pipeline.process(DetectorEvent{make_new("O1", "ACC-1", "ACME", Side::kBuy, 100.00, 500, 1001)});  // valid
    pipeline.process(DetectorEvent{make_execution("NEVER_SEEN_2", "ACC-1", "", "ACME", 100, 1002)});

    EXPECT_EQ(pipeline.inconsistent_events_skipped(), 2u);
    EXPECT_EQ(counting->call_count_, 1);  // only the valid event reached the detectors
}
