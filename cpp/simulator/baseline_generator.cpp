#include "baseline_generator.hpp"

#include <cmath>
#include <unordered_map>

#include "instrument_universe.hpp"
#include "random_utils.hpp"

namespace tse::simulator {

namespace {

constexpr const char* kVenue = "SIM";

double round_to_tick(double price, double tick_size) {
    if (tick_size <= 0.0) return price;
    return std::round(price / tick_size) * tick_size;
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
        mid = std::max(instrument.tick_size, mid + instrument.tick_size * uniform_double(rng, -2.0, 2.0));

        Side side = uniform_double(rng, 0.0, 1.0) < 0.5 ? Side::kBuy : Side::kSell;
        int64_t offset_ticks = uniform_int64(rng, 0, 3);
        double price = mid + (side == Side::kBuy ? -1.0 : 1.0) * static_cast<double>(offset_ticks) *
                                  instrument.tick_size;
        price = round_to_tick(price, instrument.tick_size);

        int64_t qty = uniform_int64(rng, 1, 10) * 100;
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

        double outcome = uniform_double(rng, 0.0, 1.0);
        if (outcome < 0.60) {
            // Fully filled shortly after.
            int64_t fill_delay_ns = uniform_int64(rng, 10'000'000, 2'000'000'000);

            const Account* counterparty = &pick_random(accounts, rng);
            for (int attempt = 0; attempt < 5 && counterparty->account_id == account.account_id; ++attempt) {
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
            for (int attempt = 0; attempt < 5 && counterparty->account_id == account.account_id; ++attempt) {
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
