#include "instrument_universe.hpp"

#include <algorithm>
#include <array>
#include <functional>

namespace tse::simulator {

namespace {

constexpr std::array<const char*, 8> kEquitySymbols = {
    "ACME", "GLBX", "NDEX", "ORCA", "PLTX", "QTRX", "RHNO", "STLR",
};
constexpr std::array<const char*, 5> kFxSymbols = {
    "EURUSD", "USDJPY", "GBPUSD", "AUDUSD", "USDCHF",
};
constexpr std::array<const char*, 5> kFixedIncomeSymbols = {
    "UST2Y", "UST5Y", "UST10Y", "UST30Y", "BUND10Y",
};

Instrument make_instrument(const char* symbol, AssetClass asset_class, double tick_size,
                            int64_t avg_daily_volume, int64_t session_close_ns) {
    Instrument instrument;
    instrument.instrument_id = symbol;
    instrument.symbol = symbol;
    instrument.asset_class = asset_class;
    instrument.tick_size = tick_size;
    instrument.avg_daily_volume = avg_daily_volume;
    instrument.session_close_ns = session_close_ns;
    return instrument;
}

}  // namespace

std::vector<Instrument> build_instrument_universe(const InstrumentUniverseConfig& config) {
    std::vector<Instrument> instruments;

    int num_equity = std::min<int>(config.num_equity, static_cast<int>(kEquitySymbols.size()));
    for (int i = 0; i < num_equity; ++i) {
        instruments.push_back(make_instrument(kEquitySymbols[i], AssetClass::kEquity, 0.01,
                                               1'000'000, config.session_close_ns));
    }

    int num_fx = std::min<int>(config.num_fx, static_cast<int>(kFxSymbols.size()));
    for (int i = 0; i < num_fx; ++i) {
        instruments.push_back(make_instrument(kFxSymbols[i], AssetClass::kFx, 0.0001,
                                               100'000'000, config.session_close_ns));
    }

    int num_fixed_income =
        std::min<int>(config.num_fixed_income, static_cast<int>(kFixedIncomeSymbols.size()));
    for (int i = 0; i < num_fixed_income; ++i) {
        instruments.push_back(make_instrument(kFixedIncomeSymbols[i], AssetClass::kFixedIncome,
                                               0.01, 200'000, config.session_close_ns));
    }

    return instruments;
}

double reference_price(const Instrument& instrument) {
    // Deterministic per-symbol jitter so instruments in the same asset class
    // don't all anchor to an identical price.
    double jitter = static_cast<double>(std::hash<std::string>{}(instrument.instrument_id) % 1000) / 1000.0;

    switch (instrument.asset_class) {
        case AssetClass::kEquity:
            return 50.0 + jitter * 150.0;  // ~50-200
        case AssetClass::kFx:
            return 0.8 + jitter * 0.8;  // ~0.8-1.6
        case AssetClass::kFixedIncome:
            return 95.0 + jitter * 10.0;  // ~95-105 (price as % of par)
    }
    return 100.0;
}

}  // namespace tse::simulator
