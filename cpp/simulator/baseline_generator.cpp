#include "baseline_generator.hpp"

#include <cmath>
#include <unordered_map>

#include "instrument_universe.hpp"
#include "random_utils.hpp"

namespace tse::simulator {

namespace {

constexpr const char* kVenue = "SIM";

// Empirical trade-size percentiles, WRDS TAQ (calibration/output/stylized_facts.json,
// trade_size.aggregate, n=1,421,575 real prints, 10-name basket
// [AAPL,MSFT,JPM,XOM,TGT,GILD,DE,EBAY,HAS,RF], NBBO-corrected quotes
// paired with trades pulled 2026-03-11/2026-03-25/2026-04-01/2026-04-08,
// 11:00-12:30 midday window). Replaces the previous flat uniform(100,1000)
// -- real prints are dominated by small odd-lot prints (median 20 shares,
// not 100+) with a long right tail, not a uniform block of round lots.
//
// This table's trapezoidal mean (53.275) also feeds
// abuse/marking_the_close.cpp's kMeanBaselineExecutionQty (via the fill
// split below) -- a change to this table's shape should also update that
// constant, or its ambient-volume anchoring goes stale.
const std::vector<std::pair<double, int64_t>> kTradeSizePercentiles = {
    {0.00, 1}, {0.10, 1}, {0.25, 4}, {0.50, 20}, {0.75, 50}, {0.90, 100}, {0.99, 460}, {1.00, 460},
};

// Empirical |price change| between consecutive trades, in ticks
// (calibration/output/stylized_facts.json, intraday_price_volatility.aggregate,
// n=1,417,125, same basis as kTradeSizePercentiles -- the sym_suffix-corrected
// final pull, after excluding JPM/RF preferred-share rows that had been
// merged into the common-stock series by a query bug; see
// PARAMETER_MAPPING.md's "Known pitfalls" section for the full story).
// mean is NOT used as a calibration input -- only the percentile shape.
const std::vector<std::pair<double, int64_t>> kPriceMoveMagnitudePercentiles = {
    {0.00, 0}, {0.10, 0}, {0.25, 0}, {0.50, 0}, {0.75, 1}, {0.90, 4}, {0.99, 28}, {1.00, 28},
};

// Empirical bid-ask spread, WRDS TAQ (calibration/output/stylized_facts.json,
// spread.aggregate, n=2,166,255, same corrected-NBBO basis as the two
// tables above). LIQUID TIER ONLY -- this is the trade-count-dominant
// tier of the 10-name basket (AAPL/MSFT/XOM/RF, ~77% of trade count,
// p50 1-3 ticks), not a universal table. DE/HAS/TGT/JPM/GILD/EBAY are
// materially less liquid (DE's own p10=28 ticks sits above this table's
// entire representable range) and are NOT represented here -- see
// PARAMETER_MAPPING.md's "Deferred: Instrument Liquidity Tiering" for
// the follow-on work and why it wasn't folded into this row.
//
// The generator draws each order's distance from mid independently per
// order (no paired bid/ask), so there's no exact calibration target for
// a single offset -- this table is real spread/2, floored to ticks, so
// that two independently drawn offsets (one per side) sum to roughly
// the real quoted spread. Capped at p99/2=10 rather than half the true
// max (178/2=89) -- same precedent as the two tables above.
const std::vector<std::pair<double, int64_t>> kSpreadOffsetPercentiles = {
    {0.00, 0}, {0.10, 0}, {0.25, 1}, {0.50, 1}, {0.75, 2}, {0.90, 3}, {0.99, 10}, {1.00, 10},
};

double round_to_tick(double price, double tick_size) {
    if (tick_size <= 0.0) return price;
    return std::round(price / tick_size) * tick_size;
}

// Mirrors cpp/detectors/account_registry.cpp's AccountRegistry::is_related()
// (self-trade, same beneficial owner, or an explicit link) -- deliberately
// duplicated rather than shared, since this is generation-time simulator
// code and that is runtime detector code, kept independent per this
// project's ml_service/dashboard-independence precedent applied to
// simulator/detectors too. A real bug found via Phase 11 (not a Row 6 bug
// itself -- Row 6 just made it visible at realistic order volume): baseline
// flow's counterparty selection only ever re-drew to avoid an exact
// self-match, never checked relatedness, so a random counterparty draw
// could coincidentally land on the account's own linked partner and get
// recorded as a genuine two-sided execution -- indistinguishable from a
// real wash trade to WashTradeDetector, which is working exactly as
// designed. See PARAMETER_MAPPING.md for the quantified before/after.
bool accounts_related(const Account& a, const Account& b) {
    if (a.account_id == b.account_id) return true;
    if (!a.beneficial_owner_id.empty() && a.beneficial_owner_id == b.beneficial_owner_id) return true;
    for (const auto& linked_id : a.linked_account_ids) {
        if (linked_id == b.account_id) return true;
    }
    for (const auto& linked_id : b.linked_account_ids) {
        if (linked_id == a.account_id) return true;
    }
    return false;
}

}  // namespace

BaselineOutput generate_baseline_flow(const BaselineConfig& config,
                                       const std::vector<Instrument>& instruments,
                                       const std::vector<Account>& accounts, std::mt19937_64& rng,
                                       IdGenerator& order_id_gen, IdGenerator& trade_id_gen) {
    BaselineOutput output;
    if (instruments.empty() || accounts.empty()) return output;

    std::unordered_map<std::string, double> mid_price;
    for (const auto& instrument : instruments) {
        mid_price[instrument.instrument_id] = reference_price(instrument);
    }

    int64_t t = config.start_time_ns;
    while (true) {
        t += poisson_interarrival_ns(rng, config.orders_per_second);
        if (t >= config.end_time_ns) break;

        const Instrument& instrument = pick_random(instruments, rng);
        const Account& account = pick_random(accounts, rng);

        double& mid = mid_price[instrument.instrument_id];
        int64_t move_ticks = sample_from_percentiles(rng, kPriceMoveMagnitudePercentiles);
        double move_sign = uniform_double(rng, 0.0, 1.0) < 0.5 ? -1.0 : 1.0;
        mid = std::max(instrument.tick_size,
                        mid + move_sign * static_cast<double>(move_ticks) * instrument.tick_size);

        Side side = uniform_double(rng, 0.0, 1.0) < 0.5 ? Side::kBuy : Side::kSell;
        int64_t offset_ticks = sample_from_percentiles(rng, kSpreadOffsetPercentiles);
        double price = mid + (side == Side::kBuy ? -1.0 : 1.0) * static_cast<double>(offset_ticks) *
                                  instrument.tick_size;
        price = round_to_tick(price, instrument.tick_size);

        int64_t qty = sample_from_percentiles(rng, kTradeSizePercentiles);
        OrderType order_type = uniform_double(rng, 0.0, 1.0) < 0.1 ? OrderType::kMarket : OrderType::kLimit;

        Order order;
        order.order_id = order_id_gen.next();
        order.account_id = account.account_id;
        order.instrument_id = instrument.instrument_id;
        order.side = side;
        order.price = price;
        order.qty = qty;
        order.order_type = order_type;
        order.timestamp_ns = t;
        order.status = OrderStatus::kNew;
        order.venue = kVenue;
        output.orders.push_back(order);

        // 0.60/0.15/0.15/0.10 fill/partial/cancel/open split -- also feeds
        // abuse/marking_the_close.cpp's kBaselineExecutionProbability (0.75
        // = 0.60+0.15) and, combined with kTradeSizePercentiles above,
        // kMeanBaselineExecutionQty. A change to this split should update
        // both, or that file's ambient-volume anchoring goes stale.
        double outcome = uniform_double(rng, 0.0, 1.0);
        if (outcome < 0.60) {
            // Fully filled shortly after.
            int64_t fill_delay_ns = uniform_int64(rng, 10'000'000, 2'000'000'000);

            const Account* counterparty = &pick_random(accounts, rng);
            for (int attempt = 0; attempt < 5 && accounts_related(*counterparty, account); ++attempt) {
                counterparty = &pick_random(accounts, rng);
            }

            Execution execution;
            execution.trade_id = trade_id_gen.next();
            execution.order_id = order.order_id;
            execution.account_id = order.account_id;
            execution.instrument_id = order.instrument_id;
            execution.price = order.price;
            execution.qty = order.qty;
            execution.timestamp_ns = t + fill_delay_ns;
            execution.counterparty_account_id = counterparty->account_id;
            execution.venue = kVenue;
            output.executions.push_back(execution);
        } else if (outcome < 0.75) {
            // Partially filled, remains open — no further record needed.
            int64_t fill_delay_ns = uniform_int64(rng, 10'000'000, 2'000'000'000);
            int64_t fill_qty = std::max<int64_t>(1, order.qty / 2);

            const Account* counterparty = &pick_random(accounts, rng);
            for (int attempt = 0; attempt < 5 && accounts_related(*counterparty, account); ++attempt) {
                counterparty = &pick_random(accounts, rng);
            }

            Execution execution;
            execution.trade_id = trade_id_gen.next();
            execution.order_id = order.order_id;
            execution.account_id = order.account_id;
            execution.instrument_id = order.instrument_id;
            execution.price = order.price;
            execution.qty = fill_qty;
            execution.timestamp_ns = t + fill_delay_ns;
            execution.counterparty_account_id = counterparty->account_id;
            execution.venue = kVenue;
            output.executions.push_back(execution);
        } else if (outcome < 0.90) {
            // Cancelled after a dwell period.
            int64_t dwell_ns = uniform_int64(rng, 500'000'000, 30'000'000'000);

            Order cancel = order;
            cancel.status = OrderStatus::kCancelled;
            cancel.timestamp_ns = t + dwell_ns;
            output.orders.push_back(cancel);
        }
        // else: remains open for the rest of the session — no further record.
    }

    return output;
}

}  // namespace tse::simulator
