#!/usr/bin/env python3
"""Computes stylized-fact summary statistics from a real WRDS/TAQ export.

Consumes exactly the trades.csv/quotes.csv schema documented in
../PARAMETER_MAPPING.md and writes only the derived numbers (percentiles,
ratios) to output/stylized_facts.json -- never the underlying rows, per
CLAUDE.md's rule that no raw or derived WRDS/TAQ data (beyond these
summary statistics) is ever committed to the repo.

Usage:
    python3 compute_stylized_facts.py trades.csv quotes.csv [--out PATH]
    python3 compute_stylized_facts.py --selftest

Stdlib only, deliberately -- this is a small, one-shot analysis script,
not worth a venv/requirements.txt of its own (see ml_service/ for where
that tradeoff goes the other way).
"""
import argparse
import csv
import datetime
import json
import statistics
import sys
from collections import defaultdict

# Standard US equity tick size post-decimalization. Real WRDS TAQ exports
# don't carry tick size directly -- this is a documented simplifying
# assumption (all the candidate names in PARAMETER_MAPPING.md's suggested
# basket are sub-$1000 common stocks quoted in whole cents), not a value
# derived from the data itself.
EQUITY_TICK_SIZE = 0.01

PERCENTILES = (10, 25, 50, 75, 90, 99)


def _percentiles(values):
    if not values:
        return {}
    values = sorted(values)
    n = len(values)
    result = {}
    for p in PERCENTILES:
        idx = min(n - 1, max(0, round((p / 100.0) * (n - 1))))
        result[f"p{p}"] = values[idx]
    result["mean"] = statistics.fmean(values)
    result["count"] = n
    return result


def _parse_time(value):
    # Tolerates HH:MM:SS.sss or HH:MM:SS.ffffff -- TAQ exports vary by product.
    for fmt in ("%H:%M:%S.%f", "%H:%M:%S"):
        try:
            return datetime.datetime.strptime(value.strip(), fmt)
        except ValueError:
            continue
    raise ValueError(f"unparseable time: {value!r}")


def read_trades(path):
    rows = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            rows.append(
                {
                    "symbol": row["symbol"],
                    "date": row["date"],
                    "time": _parse_time(row["time"]),
                    "price": float(row["price"]),
                    "size": int(float(row["size"])),
                }
            )
    return rows


def read_quotes(path):
    rows = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            rows.append(
                {
                    "symbol": row["symbol"],
                    "date": row["date"],
                    "time": _parse_time(row["time"]),
                    "bid": float(row["bid"]),
                    "ask": float(row["ask"]),
                }
            )
    return rows


def compute_trade_size_stats(trades):
    by_symbol = defaultdict(list)
    for t in trades:
        by_symbol[t["symbol"]].append(t["size"])
    all_sizes = [t["size"] for t in trades]
    return {
        "aggregate": _percentiles(all_sizes),
        "by_symbol": {sym: _percentiles(sizes) for sym, sizes in by_symbol.items()},
    }


def compute_spread_stats(quotes):
    by_symbol = defaultdict(list)
    all_spreads_ticks = []
    for q in quotes:
        spread = q["ask"] - q["bid"]
        if spread <= 0:
            continue  # crossed/locked quote artifact -- exclude, not a real spread
        ticks = spread / EQUITY_TICK_SIZE
        by_symbol[q["symbol"]].append(ticks)
        all_spreads_ticks.append(ticks)
    return {
        "unit": "ticks (assumes $0.01 equity tick size -- see EQUITY_TICK_SIZE)",
        "aggregate": _percentiles(all_spreads_ticks),
        "by_symbol": {sym: _percentiles(v) for sym, v in by_symbol.items()},
    }


def compute_volatility_stats(trades):
    # Average absolute price change between consecutive trades, per
    # (symbol, date) -- directly consumable as "how many ticks does the
    # price move per print," the same unit baseline_generator.cpp's price
    # walk step needs.
    by_key = defaultdict(list)
    for t in trades:
        by_key[(t["symbol"], t["date"])].append(t)
    abs_changes_ticks = []
    for key, group in by_key.items():
        group.sort(key=lambda t: t["time"])
        for a, b in zip(group, group[1:]):
            abs_changes_ticks.append(abs(b["price"] - a["price"]) / EQUITY_TICK_SIZE)
    return {
        "unit": "abs price change between consecutive trades, in ticks",
        "aggregate": _percentiles(abs_changes_ticks),
    }


def compute_quote_to_trade_ratio(trades, quotes):
    # Standard market-microstructure proxy for order-cancellation/churn
    # activity -- TAQ has no raw cancel messages (see PARAMETER_MAPPING.md's
    # caveat), so this is an indirect stand-in, not a literal cancel rate.
    trade_counts = defaultdict(int)
    quote_counts = defaultdict(int)
    for t in trades:
        trade_counts[(t["symbol"], t["date"])] += 1
    for q in quotes:
        quote_counts[(q["symbol"], q["date"])] += 1
    ratios = []
    for key, n_trades in trade_counts.items():
        if n_trades == 0:
            continue
        ratios.append(quote_counts.get(key, 0) / n_trades)
    return {
        "description": "quote updates per trade -- a proxy for order churn/"
        "cancellation, not a direct cancellation rate (TAQ has no order-level "
        "cancel messages)",
        "aggregate": _percentiles(ratios),
    }


def compute_all(trades, quotes):
    session_seconds = 6.5 * 3600  # standard US equity regular session
    return {
        "trade_size": compute_trade_size_stats(trades),
        "spread": compute_spread_stats(quotes),
        "intraday_price_volatility": compute_volatility_stats(trades),
        "quote_to_trade_ratio_cancellation_proxy": compute_quote_to_trade_ratio(trades, quotes),
        "order_arrival_rate_proxy_per_second": (len(trades) + len(quotes))
        / max(1, len({(t["symbol"], t["date"]) for t in trades}))
        / session_seconds,
    }


def run_selftest():
    """Generates synthetic (obviously fake) data in memory -- never writes a
    CSV to disk -- and checks the pipeline produces sane, non-crashing
    output. Proves the script works before any real WRDS export exists."""
    import random

    rng = random.Random(42)
    trades = []
    quotes = []
    base_price = 100.00
    for i in range(500):
        time_str = f"9:{(30 + i // 20) % 60:02d}:{i % 60:02d}.{i % 1000:03d}"
        price = base_price + rng.uniform(-0.5, 0.5)
        trades.append(
            {
                "symbol": "FAKE",
                "date": "2026-01-01",
                "time": _parse_time(time_str),
                "price": price,
                "size": rng.choice([100, 100, 100, 200, 500, 1000]),
            }
        )
        quotes.append(
            {
                "symbol": "FAKE",
                "date": "2026-01-01",
                "time": _parse_time(time_str),
                "bid": price - 0.01,
                "ask": price + 0.01,
            }
        )

    result = compute_all(trades, quotes)

    def assert_monotonic_percentiles(stats, name):
        # A real invariant of _percentiles() itself, not tied to this
        # synthetic data's specific values -- would catch a broken
        # percentile computation (e.g. an unsorted-input regression)
        # regardless of what data is fed in, real or fake.
        ordered = [stats[f"p{p}"] for p in PERCENTILES]
        assert ordered == sorted(ordered), f"{name}: percentiles not monotonic: {ordered}"

    # Shape checks -- these ARE tied to this synthetic run's specific
    # construction (500 trades, 499 consecutive-pairs, 1 quote per trade).
    assert result["trade_size"]["aggregate"]["count"] == 500
    assert result["spread"]["aggregate"]["count"] == 500
    assert result["intraday_price_volatility"]["aggregate"]["count"] == 499
    assert result["quote_to_trade_ratio_cancellation_proxy"]["aggregate"]["p50"] == 1.0

    # Real sanity bounds on the VALUES, not just their shape -- these
    # should hold on any real WRDS export too, not just this fake data,
    # since they're physical/definitional constraints (a spread can't be
    # negative, sizes can't be negative, percentiles must be ordered).
    assert_monotonic_percentiles(result["trade_size"]["aggregate"], "trade_size")
    assert_monotonic_percentiles(result["spread"]["aggregate"], "spread")
    assert_monotonic_percentiles(result["intraday_price_volatility"]["aggregate"], "volatility")
    assert result["trade_size"]["aggregate"]["p10"] > 0, "trade sizes must be positive"
    assert result["spread"]["aggregate"]["p10"] > 0, "spreads must be positive (crossed quotes should already be filtered)"
    assert result["spread"]["aggregate"]["p99"] < 10_000, "spread of >10,000 ticks is almost certainly a unit/parsing bug, not a real quote"
    assert result["intraday_price_volatility"]["aggregate"]["p10"] >= 0, "abs price changes can't be negative"
    assert result["quote_to_trade_ratio_cancellation_proxy"]["aggregate"]["p50"] > 0, "some quote activity should exist relative to trades in any real market"

    print("self-test passed: pipeline runs end-to-end on synthetic data, shape AND value sanity bounds hold")
    print(json.dumps(result, indent=2, default=str)[:500] + "\n...")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trades_csv", nargs="?")
    parser.add_argument("quotes_csv", nargs="?")
    parser.add_argument("--out", default="output/stylized_facts.json")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    if args.selftest:
        run_selftest()
        return

    if not args.trades_csv or not args.quotes_csv:
        parser.error("trades_csv and quotes_csv are required unless --selftest is given")

    trades = read_trades(args.trades_csv)
    quotes = read_quotes(args.quotes_csv)
    if not trades or not quotes:
        sys.exit("no rows read from one or both input files -- check the export matches "
                  "PARAMETER_MAPPING.md's schema")

    result = compute_all(trades, quotes)
    with open(args.out, "w") as f:
        json.dump(result, f, indent=2, default=str)
    print(f"wrote {args.out} ({len(trades)} trades, {len(quotes)} quotes summarized -- "
          f"no raw rows written)")


if __name__ == "__main__":
    main()
