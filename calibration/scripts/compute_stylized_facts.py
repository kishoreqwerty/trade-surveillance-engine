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
    # min/max are cheap (values already sorted) and matter specifically
    # when mean is far from p99 -- that combination only happens with a
    # tail beyond p99, and min/max is how far out it actually goes,
    # not just that it exists.
    result["min"] = values[0]
    result["max"] = values[-1]
    return result


def _parse_time(value):
    # Tolerates HH:MM:SS, HH:MM:SS.sss, HH:MM:SS.ffffff (microseconds --
    # WRDS Daily TAQ's actual TIME_M format), and anything with MORE than 6
    # fractional digits (nanosecond-precision TAQ products): Python's %f
    # only accepts up to 6 digits, so longer fractions are truncated to
    # microsecond precision rather than raising -- sub-microsecond
    # resolution doesn't matter for the second/minute-scale stats this
    # script computes, but silently mis-truncating without trying would.
    value = value.strip()
    if "." in value:
        whole, frac = value.split(".", 1)
        if len(frac) > 6:
            value = f"{whole}.{frac[:6]}"
    for fmt in ("%H:%M:%S.%f", "%H:%M:%S"):
        try:
            return datetime.datetime.strptime(value, fmt)
        except ValueError:
            continue
    raise ValueError(f"unparseable time: {value!r}")


# Real WRDS Daily TAQ column names (found empirically against an actual
# WRDS export -- CT/trades uses SYM_ROOT/TIME_M, CQ/NBBO uses
# SYM_ROOT/TIME_M/BIDSIZ/ASKSIZ, all lowercased by WRDS's CSV export).
# PARAMETER_MAPPING.md's originally-documented "symbol"/"time"/"bidsize"/
# "asksize" schema was this project's best guess before anyone had actually
# queried WRDS -- wrong, and now corrected in both places. Internal
# representation below (the "symbol"/"time" dict keys every compute_*
# function reads) is kept as the neutral, source-agnostic name; only this
# read boundary needs to know the real column names.
_TRADE_COLUMNS = {"symbol": "sym_root", "date": "date", "time": "time_m", "price": "price", "size": "size"}
_QUOTE_COLUMNS = {
    "symbol": "sym_root",
    "date": "date",
    "time": "time_m",
    "bid": "bid",
    "ask": "ask",
    "bidsiz": "bidsiz",
    "asksiz": "asksiz",
}


def read_trades(path):
    # A 0-share row is never real traded volume and must never enter the
    # size distribution -- dropped here, at the read boundary, not deep
    # inside compute_trade_size_stats, so every caller gets the same
    # guarantee.
    #
    # Root cause, confirmed against a real WRDS export via tr_corr/
    # tr_scond crosstab (an earlier version of this comment guessed
    # "correction/cancellation records," which the same crosstab then
    # RULED OUT -- 100% of size<=0 rows carry tr_corr=='00', i.e.
    # uncorrected/normal, not a correction): 95.5% carry sale condition
    # 'I' (odd lot indicator) -- a known TAQ reporting quirk where certain
    # odd-lot prints report size=0 in the SIZE field instead of the actual
    # (sub-round-lot) share count. This also explains why the rate was
    # higher in AAPL/MSFT specifically: odd-lot trades cluster in the most
    # heavily-traded mega-caps. See PARAMETER_MAPPING.md's "Known
    # pitfalls" section for the full writeup. The filter itself needed no
    # change once this was known -- a size=0 row was never usable trade-
    # size data regardless of which of the two explanations turned out to
    # be true.
    #
    # dropped is returned alongside the rows so a caller (main()) can
    # report how much of the raw export this filter removes.
    rows = []
    dropped = 0
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            size = int(float(row[_TRADE_COLUMNS["size"]]))
            if size <= 0:
                dropped += 1
                continue
            rows.append(
                {
                    "symbol": row[_TRADE_COLUMNS["symbol"]],
                    "date": row[_TRADE_COLUMNS["date"]],
                    "time": _parse_time(row[_TRADE_COLUMNS["time"]]),
                    "price": float(row[_TRADE_COLUMNS["price"]]),
                    "size": size,
                }
            )
    return rows, dropped


def read_quotes(path):
    # bidsiz/asksiz == 0 marks a one-sided or withdrawn quote -- the price
    # is populated but there's no real size resting there, so it isn't a
    # genuine two-sided market and must not be paired into a spread.
    # Dropped here for the same reason size <= 0 is dropped in
    # read_trades(): at the read boundary, with a returned count, not
    # buried inside compute_spread_stats().
    #
    # This does NOT, by itself, fix a raw-per-exchange-quotes-instead-of-
    # NBBO problem (see PARAMETER_MAPPING.md's "Known spread pitfall" —
    # a single venue's own two-sided quote can be genuinely zero-size-
    # filtered-clean and still be a wide, non-competitive, defensive quote
    # that was never close to the real inside market). This filter and
    # that problem are independent; both matter.
    rows = []
    dropped_zero_size = 0
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            bidsiz = float(row[_QUOTE_COLUMNS["bidsiz"]]) if _QUOTE_COLUMNS["bidsiz"] in row else 1.0
            asksiz = float(row[_QUOTE_COLUMNS["asksiz"]]) if _QUOTE_COLUMNS["asksiz"] in row else 1.0
            if bidsiz <= 0 or asksiz <= 0:
                dropped_zero_size += 1
                continue
            rows.append(
                {
                    "symbol": row[_QUOTE_COLUMNS["symbol"]],
                    "date": row[_QUOTE_COLUMNS["date"]],
                    "time": _parse_time(row[_QUOTE_COLUMNS["time"]]),
                    "bid": float(row[_QUOTE_COLUMNS["bid"]]),
                    "ask": float(row[_QUOTE_COLUMNS["ask"]]),
                }
            )
    return rows, dropped_zero_size


def diagnose_zero_size_trades(path):
    """Characterizes zero-size trade rows using only sym_root/date/time_m/
    price/size -- no trade-condition-code column needed, since the
    current trades.csv schema doesn't have one. Answers one concrete
    question: does each zero-size row share an exact (symbol, date,
    time_m, price) with a real (positive-size) trade, i.e. does it look
    like a companion/echo/correction record tied to a specific real
    print -- or is it isolated, with no matching real trade at all, which
    would point to something else being pulled into the trades table
    entirely (not a correction of anything)?

    Run this BEFORE re-querying WRDS with a condition-code column added --
    it's immediate signal from data you already have, not a replacement
    for checking the real condition codes, but a way to narrow down what
    to expect from them.
    """
    all_rows = []
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            all_rows.append(
                {
                    "symbol": row[_TRADE_COLUMNS["symbol"]],
                    "date": row[_TRADE_COLUMNS["date"]],
                    "time": row[_TRADE_COLUMNS["time"]],  # raw string -- exact match, no parsing needed here
                    "price": float(row[_TRADE_COLUMNS["price"]]),
                    "size": int(float(row[_TRADE_COLUMNS["size"]])),
                }
            )

    zero_rows = [r for r in all_rows if r["size"] <= 0]
    nonzero_rows = [r for r in all_rows if r["size"] > 0]
    if not zero_rows:
        print("no zero-size rows found -- nothing to diagnose")
        return

    # Index real trades two ways: exact (symbol,date,time,price) for a
    # "companion record at the identical instant" check, and the looser
    # (symbol,date,price) for "same print price, sometime that day" --
    # the exact-match rate is the important number; same-price-only is a
    # weaker secondary signal.
    by_exact = defaultdict(set)
    by_symbol_date_price = defaultdict(int)
    for r in nonzero_rows:
        by_exact[(r["symbol"], r["date"], r["time"])].add(round(r["price"], 4))
        by_symbol_date_price[(r["symbol"], r["date"], round(r["price"], 4))] += 1

    exact_match = same_price_match = isolated = 0
    for r in zero_rows:
        price = round(r["price"], 4)
        if price in by_exact.get((r["symbol"], r["date"], r["time"]), ()):
            exact_match += 1
        elif by_symbol_date_price.get((r["symbol"], r["date"], price), 0) > 0:
            same_price_match += 1
        else:
            isolated += 1

    total_zero = len(zero_rows)
    print(f"zero-size trade rows: {total_zero} ({total_zero / len(all_rows):.1%} of all rows)")
    print(f"  exact (symbol,date,time,price) match to a real trade: {exact_match} ({exact_match / total_zero:.1%})")
    print(f"    -> if HIGH: these look like companion/echo/correction records tied to a specific real print")
    print(f"  same (symbol,date,price), different time: {same_price_match} ({same_price_match / total_zero:.1%})")
    print(f"  isolated -- no matching real trade at that price at all: {isolated} ({isolated / total_zero:.1%})")
    print(f"    -> if HIGH: these probably aren't corrections of anything real in this export --")
    print(f"       something else (a different message type entirely?) may be getting pulled into 'trades'")

    by_symbol_zero = defaultdict(int)
    by_symbol_total = defaultdict(int)
    for r in all_rows:
        by_symbol_total[r["symbol"]] += 1
        if r["size"] <= 0:
            by_symbol_zero[r["symbol"]] += 1
    print("\nper-symbol zero-size rate (uniform across all names -> systemic; concentrated in one/two -> "
          "name-specific, e.g. a corporate action that day):")
    for sym in sorted(by_symbol_total):
        rate = by_symbol_zero[sym] / by_symbol_total[sym]
        print(f"  {sym}: {by_symbol_zero[sym]}/{by_symbol_total[sym]} ({rate:.1%})")


def diagnose_trade_corrections(path):
    """Crosstabs tr_corr (Trade Correction Indicator) and tr_scond (Sale
    Condition) against size<=0 vs size>0, for a trades.csv re-pulled to
    include those two columns specifically to investigate the 21.6%
    zero-size rate found without them.

    Requires columns sym_root,date,time_m,price,size,tr_corr,tr_scond --
    NOT the standard trades.csv schema PARAMETER_MAPPING.md documents for
    ongoing calibration (that one deliberately has no condition-code
    columns); this is a one-time investigative export, separate from
    read_trades()'s schema on purpose.

    On '00' meaning "uncorrected/normal": real WRDS-hosted SAS example
    code filters trades with `tr_corr = '00'` to get clean prints -- see
    wrds-www.wharton.upenn.edu's taq6.sas macro -- which is good practical
    evidence for that specific value. This project could NOT retrieve the
    complete NYSE Daily TAQ Client Specification's full code table from
    either PDF version checked (both failed to extract usable text, same
    failure mode as the CFTC Sarao complaint PDF in
    cpp/simulator/abuse/sarao_case.hpp). Treat "'00' likely means
    normal/uncorrected" as reasonably well-supported, not as an
    exhaustively verified table of every other code's meaning -- this
    function reports the RAW distribution of whatever values actually
    appear in your data instead of assuming which ones matter.
    """
    from collections import Counter

    corr_zero, corr_nonzero = Counter(), Counter()
    scond_zero, scond_nonzero = Counter(), Counter()
    total_zero = total_nonzero = 0

    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        if "tr_corr" not in reader.fieldnames or "tr_scond" not in reader.fieldnames:
            sys.exit(f"{path} has no tr_corr/tr_scond columns -- this needs the re-pulled export "
                      "that includes them, not the standard trades.csv")
        for row in reader:
            size = int(float(row["size"]))
            corr = row["tr_corr"].strip()
            scond = row["tr_scond"].strip()
            if size <= 0:
                total_zero += 1
                corr_zero[corr] += 1
                scond_zero[scond] += 1
            else:
                total_nonzero += 1
                corr_nonzero[corr] += 1
                scond_nonzero[scond] += 1

    print(f"size<=0 rows: {total_zero}   size>0 rows: {total_nonzero}\n")

    print("tr_corr distribution among size<=0 rows:")
    for value, count in corr_zero.most_common():
        print(f"  {value!r}: {count} ({count / total_zero:.1%})")

    print("\ntr_corr distribution among size>0 rows (top 10):")
    for value, count in corr_nonzero.most_common(10):
        print(f"  {value!r}: {count} ({count / total_nonzero:.1%})")

    uncorrected_but_zero = corr_zero.get("00", 0)
    if uncorrected_but_zero > 0:
        print(f"\nNOTE: {uncorrected_but_zero} size<=0 rows ({uncorrected_but_zero / total_zero:.1%} of them) "
              "have tr_corr=='00'. If '00' really does mean uncorrected/normal, this count is the portion "
              "of the 21.6% that a trade-correction explanation does NOT cover -- check tr_scond below for "
              "those specifically.")
    else:
        print("\nEvery size<=0 row has a non-'00' tr_corr -- consistent with (not proof of, given the "
              "unverified full code table) the correction/cancellation-record explanation.")

    print("\ntr_scond distribution among size<=0 rows:")
    for value, count in scond_zero.most_common():
        print(f"  {value!r}: {count} ({count / total_zero:.1%})")

    print("\ntr_scond distribution among size>0 rows (top 10):")
    for value, count in scond_nonzero.most_common(10):
        print(f"  {value!r}: {count} ({count / total_nonzero:.1%})")


def compute_trade_size_stats(trades):
    by_symbol = defaultdict(list)
    for t in trades:
        by_symbol[t["symbol"]].append(t["size"])
    all_sizes = [t["size"] for t in trades]
    return {
        "aggregate": _percentiles(all_sizes),
        "by_symbol": {sym: _percentiles(sizes) for sym, sizes in by_symbol.items()},
    }


# Above this relative spread, a quote is flagged in implausible_fraction
# below for investigation -- NOT auto-dropped. 200bps (2%) is already
# generously wide for a liquid large-cap in normal midday trading; this
# threshold exists to surface how much of the sample is questionable, not
# to quietly launder the distribution by discarding whatever looks bad.
IMPLAUSIBLE_SPREAD_BPS = 200.0


def compute_spread_stats(quotes):
    by_symbol = defaultdict(list)
    all_spreads_ticks = []
    all_spreads_bps = []
    implausible_count = 0
    for q in quotes:
        spread = q["ask"] - q["bid"]
        if spread <= 0:
            continue  # crossed/locked quote artifact -- exclude, not a real spread
        midpoint = (q["ask"] + q["bid"]) / 2.0
        bps = (spread / midpoint) * 10_000.0
        ticks = spread / EQUITY_TICK_SIZE
        by_symbol[q["symbol"]].append(ticks)
        all_spreads_ticks.append(ticks)
        all_spreads_bps.append(bps)
        if bps > IMPLAUSIBLE_SPREAD_BPS:
            implausible_count += 1
    return {
        "unit": "ticks (assumes $0.01 equity tick size -- see EQUITY_TICK_SIZE)",
        "aggregate": _percentiles(all_spreads_ticks),
        "by_symbol": {sym: _percentiles(v) for sym, v in by_symbol.items()},
        # Scale-invariant view -- 185 ticks means something very different
        # for a $20 stock than a $2000 one, so ticks alone can't say
        # whether a given spread is actually implausible. bps can.
        "relative_bps": _percentiles(all_spreads_bps),
        "implausible_fraction": (implausible_count / len(all_spreads_bps)) if all_spreads_bps else 0.0,
        "implausible_threshold_bps": IMPLAUSIBLE_SPREAD_BPS,
        "implausible_note": (
            "fraction of (already zero-size-filtered) quotes with relative spread over "
            f"{IMPLAUSIBLE_SPREAD_BPS:.0f}bps. A high fraction here, on liquid large-caps in a normal "
            "midday window, is a strong signal you're looking at raw per-venue quotes (a single "
            "exchange's own defensive/backup quote, not the true NBBO) rather than a consolidated "
            "NBBO table -- see PARAMETER_MAPPING.md's 'Known spread pitfall' section. This script "
            "cannot distinguish the two without a venue/exchange column, which isn't in the current "
            "schema."
        ),
    }


def _consecutive_pairs_by_symbol_date(trades):
    """Shared traversal for both find_price_jumps() and
    compute_volatility_stats() -- sorts each (symbol, date) group by time
    and yields (symbol, date, a, b) for every consecutive pair, so the two
    functions can't drift into computing "consecutive" differently."""
    by_key = defaultdict(list)
    for t in trades:
        by_key[(t["symbol"], t["date"])].append(t)
    for (symbol, date), group in by_key.items():
        group.sort(key=lambda t: t["time"])
        for a, b in zip(group, group[1:]):
            yield symbol, date, a, b


# Above this relative price change between two CONSECUTIVE trades in the
# same symbol+date, a pair is excluded from intraday_price_volatility as
# implausible -- not tuned to catch any specific symbol. Grounded in SEC's
# Limit Up-Limit Down (LULD) mechanism: NMS Tier 1 stocks trading above $3
# get a trading pause if price moves more than 5% from a reference price
# within a short window, so a genuine single print-to-print jump exceeding
# that band, let alone several times it, essentially cannot happen during
# ordinary trading without the exchange pausing the stock first. 10% is
# double that band -- a deliberately generous margin so a real (if
# unusual) large move is never the thing excluded; found via a real WRDS
# export where JPM had a print-to-print jump of ~130% (max $289.48 on a
# stock trading in the low-mid $200s) and RF a secondary, smaller one, both
# concentrated in single symbols, not evenly distributed -- the by_symbol
# breakdown existing at all is what made that concentration visible rather
# than just an inflated aggregate mean.
IMPLAUSIBLE_PRICE_CHANGE_FRACTION = 0.10


def find_price_jumps(trades_path, top_n=20):
    """Prints the top_n largest relative-price-change consecutive-trade
    pairs across the whole export, with full context (both timestamps,
    both prices, tick and percent change) -- run this FIRST, before
    trusting compute_volatility_stats()'s exclusion filter, to see the
    actual anomalous row(s) rather than just a summary statistic. Reads
    trades.csv directly (not compute_all()'s already-computed stats)."""
    trades, _dropped = read_trades(trades_path)
    jumps = []
    for symbol, date, a, b in _consecutive_pairs_by_symbol_date(trades):
        pct = abs(b["price"] - a["price"]) / a["price"] if a["price"] > 0 else 0.0
        jumps.append((pct, symbol, date, a, b))
    jumps.sort(key=lambda j: j[0], reverse=True)

    print(f"top {min(top_n, len(jumps))} largest consecutive-trade price changes "
          f"(of {len(jumps)} consecutive pairs total):\n")
    for pct, symbol, date, a, b in jumps[:top_n]:
        tick_change = (b["price"] - a["price"]) / EQUITY_TICK_SIZE
        # a["time"]/b["time"] are datetime.datetime objects with a dummy
        # 1900-01-01 date (_parse_time only ever parses a time-of-day) --
        # .time() strips that so the printed line doesn't show a
        # meaningless date next to the real symbol/date already printed.
        print(f"{symbol} {date}: {a['time'].time()} @ {a['price']}  ->  {b['time'].time()} @ {b['price']}"
              f"   ({tick_change:+.0f} ticks, {pct:.1%})")


def compute_volatility_stats(trades):
    # Average absolute price change between consecutive trades, per
    # (symbol, date) -- directly consumable as "how many ticks does the
    # price move per print," the same unit baseline_generator.cpp's price
    # walk step needs.
    #
    # by_symbol exists specifically because of a real finding: this stat's
    # aggregate mean came out far above its own p99 on a real WRDS export.
    # The breakdown showed it wasn't evenly distributed -- it was two
    # symbols (JPM dominant, RF secondary) with implausible print-to-print
    # jumps (JPM: ~130%, effectively impossible for a genuine consecutive
    # trade pair -- see IMPLAUSIBLE_PRICE_CHANGE_FRACTION's LULD-based
    # justification). Those pairs are now excluded, with the excluded
    # count reported per symbol and in aggregate so the exclusion is never
    # silent -- a real, large, GENUINE move should still show up in a
    # symbol's dropped count near zero, not get mixed in with the ones
    # that were actually corrupt.
    abs_changes_ticks = []
    by_symbol = defaultdict(list)
    dropped_by_symbol = defaultdict(int)
    total_pairs = 0
    for symbol, _date, a, b in _consecutive_pairs_by_symbol_date(trades):
        total_pairs += 1
        pct = abs(b["price"] - a["price"]) / a["price"] if a["price"] > 0 else 0.0
        if pct > IMPLAUSIBLE_PRICE_CHANGE_FRACTION:
            dropped_by_symbol[symbol] += 1
            continue
        change = abs(b["price"] - a["price"]) / EQUITY_TICK_SIZE
        abs_changes_ticks.append(change)
        by_symbol[symbol].append(change)
    total_dropped = sum(dropped_by_symbol.values())
    return {
        "unit": "abs price change between consecutive trades, in ticks",
        "aggregate": _percentiles(abs_changes_ticks),
        "by_symbol": {sym: _percentiles(v) for sym, v in by_symbol.items()},
        "implausible_pairs_dropped": total_dropped,
        "implausible_pairs_total": total_pairs,
        "implausible_pairs_dropped_by_symbol": dict(dropped_by_symbol),
        "implausible_threshold_fraction": IMPLAUSIBLE_PRICE_CHANGE_FRACTION,
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


def _compute_window_seconds(trades, quotes):
    # Empirically derive the actual pulled time window (per symbol-day,
    # averaged) instead of assuming a fixed session length. Replaces a real
    # bug found and fixed here: an earlier version hardcoded
    # `6.5 * 3600` ("standard US equity regular session") as the divisor
    # for order_arrival_rate_proxy_per_second, but the data this project
    # actually pulls is scoped to a midday window (11:00-12:30, ~90min,
    # confirmed against the real trades.csv's own time_m range), not a full
    # session -- silently understating the rate by 23400/5400 = 4.33x. See
    # PARAMETER_MAPPING.md's "Known pitfalls" section for the full story.
    # Deliberately not replaced with a second hardcoded constant (e.g.
    # `90*60`): deriving it from the data's own min/max timestamps means
    # this stays correct even if a future pull uses a different window.
    # Combines trades and quotes (not trades alone) because both define the
    # same real pulled window, and using only one risks clipping the
    # window's true edges if that one happens to have no row exactly at the
    # boundary.
    bounds = {}
    for row in trades:
        key = (row["symbol"], row["date"])
        t = row["time"]
        lo, hi = bounds.get(key, (t, t))
        bounds[key] = (min(lo, t), max(hi, t))
    for row in quotes:
        key = (row["symbol"], row["date"])
        t = row["time"]
        lo, hi = bounds.get(key, (t, t))
        bounds[key] = (min(lo, t), max(hi, t))
    if not bounds:
        return 0.0
    spans = [(hi - lo).total_seconds() for lo, hi in bounds.values()]
    return sum(spans) / len(spans)


def compute_all(trades, quotes):
    window_seconds = _compute_window_seconds(trades, quotes)
    num_symbol_days = max(1, len({(t["symbol"], t["date"]) for t in trades}))
    arrival_rate = (
        (len(trades) + len(quotes)) / num_symbol_days / window_seconds if window_seconds > 0 else 0.0
    )
    return {
        "trade_size": compute_trade_size_stats(trades),
        "spread": compute_spread_stats(quotes),
        "intraday_price_volatility": compute_volatility_stats(trades),
        "quote_to_trade_ratio_cancellation_proxy": compute_quote_to_trade_ratio(trades, quotes),
        "order_arrival_rate_proxy_per_second": {
            "description": "(trades + quote updates) per symbol-day, divided by the actual "
            "observed pull window (per symbol-day, averaged) -- not a fixed session length. "
            "See PARAMETER_MAPPING.md for why a fixed-session divisor was wrong.",
            "value": arrival_rate,
            "window_seconds_used": window_seconds,
        },
    }


def _run_csv_roundtrip_selftest():
    """Writes tiny CSVs using the REAL WRDS column names (sym_root,
    time_m, bidsiz/asksiz -- not the "symbol"/"time"/"bidsize" this
    project originally guessed before anyone had queried WRDS) to a
    tempdir, reads them back through read_trades()/read_quotes(), and
    checks the parsed values. This is the one thing the in-memory
    selftest below never exercised -- it builds dicts directly and skips
    read_trades()/read_quotes() entirely, which is exactly why a real
    WRDS export's actual column names (sym_root, not symbol; time_m, not
    time) shipped broken with a KeyError the first time real data hit it.
    Written to a tempdir specifically so nothing here ever risks looking
    like a committed WRDS/TAQ fixture."""
    import tempfile
    from pathlib import Path

    with tempfile.TemporaryDirectory() as tmp:
        trades_path = Path(tmp) / "trades.csv"
        quotes_path = Path(tmp) / "quotes.csv"
        trades_path.write_text(
            "sym_root,date,time_m,price,size\n"
            "AAPL,2026-06-03,11:00:01.123456,195.42,100\n"
            "AAPL,2026-06-03,11:00:02.654321,195.44,200\n"
        )
        # Includes one zero-size trade and one zero-size quote -- real,
        # documented TAQ artifacts (correction/cancellation records; a
        # withdrawn/one-sided quote), not hypothetical -- to prove the
        # filters added after a real WRDS export showed p10/p25 == 0 for
        # trade size actually remove them, not just that the happy path
        # parses.
        trades_path.write_text(
            "sym_root,date,time_m,price,size\n"
            "AAPL,2026-06-03,11:00:01.123456,195.42,100\n"
            "AAPL,2026-06-03,11:00:02.654321,195.44,200\n"
            "AAPL,2026-06-03,11:00:03.000000,195.44,0\n"  # correction/cancellation-style zero-size row
        )
        quotes_path.write_text(
            "sym_root,date,time_m,bid,ask,bidsiz,asksiz\n"
            "AAPL,2026-06-03,11:00:00.999999,195.41,195.43,300,300\n"
            # Extra fractional digits (nanosecond-precision TAQ) -- proves
            # _parse_time()'s truncation path, not just the 6-digit case.
            "AAPL,2026-06-03,11:00:01.500000123,195.42,195.44,300,300\n"
            "AAPL,2026-06-03,11:00:02.000000,195.40,195.50,0,300\n"  # zero bidsiz -- one-sided, not a real market
        )

        trades, trades_dropped = read_trades(str(trades_path))
        quotes, quotes_dropped = read_quotes(str(quotes_path))

        assert len(trades) == 2, f"expected 2 trades after filtering, got {len(trades)}"
        assert trades_dropped == 1, f"expected 1 zero-size trade dropped, got {trades_dropped}"
        assert all(t["size"] > 0 for t in trades), "no zero/negative-size trade should survive filtering"
        assert trades[0]["symbol"] == "AAPL"
        assert trades[0]["price"] == 195.42
        assert trades[0]["size"] == 100
        assert trades[0]["time"].hour == 11 and trades[0]["time"].minute == 0

        assert len(quotes) == 2, f"expected 2 quotes after filtering, got {len(quotes)}"
        assert quotes_dropped == 1, f"expected 1 zero-bidsiz quote dropped, got {quotes_dropped}"
        assert quotes[0]["bid"] == 195.41
        assert quotes[1]["time"].microsecond == 500000, "nanosecond-precision TIME_M should truncate to microseconds"

    print("CSV round-trip self-test passed: read_trades()/read_quotes() correctly parse real WRDS column names "
          "AND correctly filter zero-size trades/quotes")


def _run_diagnose_selftest():
    """Proves diagnose_zero_size_trades() actually discriminates the two
    cases it's meant to distinguish -- not just that it runs. One
    zero-size row is a companion to a real trade at the exact same
    (symbol,date,time,price); the other has no matching real trade at
    all. Captures stdout to check the reported counts match, since the
    function is a print-based report, not a return-value API (deliberate
    -- it's an investigative tool for a human to read, not something
    another function consumes programmatically)."""
    import contextlib
    import io
    import tempfile
    from pathlib import Path

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "trades.csv"
        path.write_text(
            "sym_root,date,time_m,price,size\n"
            "AAPL,2026-06-03,11:00:01.000000,195.42,100\n"   # real trade
            "AAPL,2026-06-03,11:00:01.000000,195.42,0\n"     # companion: exact match
            "AAPL,2026-06-03,11:00:05.000000,197.00,0\n"     # isolated: no real trade at 197.00 that day
        )
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            diagnose_zero_size_trades(str(path))
        output = buf.getvalue()

    assert "zero-size trade rows: 2" in output, output
    assert "exact (symbol,date,time,price) match to a real trade: 1 (50.0%)" in output, output
    assert "isolated -- no matching real trade at that price at all: 1 (50.0%)" in output, output
    print("diagnose_zero_size_trades self-test passed: correctly distinguishes companion-record vs isolated rows")


def _run_diagnose_corrections_selftest():
    """Proves diagnose_trade_corrections() correctly crosstabs tr_corr/
    tr_scond against size<=0 -- three rows: one normal (size>0, tr_corr
    '00'), one size<=0 with tr_corr '00' (the "correction theory doesn't
    cover this one" case), one size<=0 with a non-'00' tr_corr."""
    import contextlib
    import io
    import tempfile
    from pathlib import Path

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "trades_with_corr.csv"
        path.write_text(
            "sym_root,date,time_m,price,size,tr_corr,tr_scond\n"
            "AAPL,2026-06-03,11:00:01.000000,195.42,100,00,@ T\n"
            "AAPL,2026-06-03,11:00:02.000000,195.44,0,00,@ T\n"
            "AAPL,2026-06-03,11:00:03.000000,195.44,0,12,@ TI\n"
        )
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            diagnose_trade_corrections(str(path))
        output = buf.getvalue()

    assert "size<=0 rows: 2   size>0 rows: 1" in output, output
    assert "'00': 1 (50.0%)" in output, output  # among size<=0 rows
    assert "'12': 1 (50.0%)" in output, output
    assert "NOTE: 1 size<=0 rows (50.0% of them) have tr_corr=='00'" in output, output
    print("diagnose_trade_corrections self-test passed: correctly crosstabs tr_corr/tr_scond against size<=0")


def _run_price_jump_filter_selftest():
    """Proves compute_volatility_stats()'s implausible-pair filter does
    BOTH things it needs to: excludes a print-to-print jump like the real
    JPM finding (~130%, effectively impossible), AND -- the part that
    actually matters, since a filter that excludes too much is just as
    wrong as one that excludes too little -- preserves a large but
    legitimate move (8%, under the 10% threshold) rather than silently
    dropping it too."""
    trades = [
        # BAD: normal small move, then a ~130% jump straight back down --
        # the JPM pattern (a corrupted/mispaired print), must be excluded.
        {"symbol": "BAD", "date": "2026-06-03", "time": _parse_time("11:00:00.000000"), "price": 220.00, "size": 100},
        {"symbol": "BAD", "date": "2026-06-03", "time": _parse_time("11:00:01.000000"), "price": 220.10, "size": 100},
        {"symbol": "BAD", "date": "2026-06-03", "time": _parse_time("11:00:02.000000"), "price": 509.48, "size": 100},
        {"symbol": "BAD", "date": "2026-06-03", "time": _parse_time("11:00:03.000000"), "price": 220.20, "size": 100},
        # GOOD: one legitimate 8% jump -- under the 10% threshold, must survive.
        {"symbol": "GOOD", "date": "2026-06-03", "time": _parse_time("11:00:00.000000"), "price": 100.00, "size": 100},
        {"symbol": "GOOD", "date": "2026-06-03", "time": _parse_time("11:00:01.000000"), "price": 108.00, "size": 100},
    ]
    result = compute_volatility_stats(trades)

    assert result["implausible_pairs_total"] == 4, result  # BAD: 4 trades -> 3 pairs; GOOD: 2 trades -> 1 pair
    assert result["implausible_pairs_dropped"] == 2, "both jumps touching the 509.48 outlier must be dropped"
    assert result["implausible_pairs_dropped_by_symbol"] == {"BAD": 2}, "GOOD's legitimate 8% move must not be dropped"
    assert "GOOD" in result["by_symbol"], "GOOD symbol must still have volatility data"
    good_changes = result["by_symbol"]["GOOD"]
    assert good_changes["max"] == 800.0, f"GOOD's real 8%%/800-tick move must survive the filter, got {good_changes}"
    print("price-jump-filter self-test passed: excludes the implausible pair, preserves the legitimate large move")


def _run_arrival_rate_window_selftest():
    """Proves _compute_window_seconds() derives the real pulled window from
    the data's own timestamps rather than assuming a fixed session length --
    the exact bug this replaced (hardcoded 6.5h divisor against data that
    only ever spans ~90min, understating the rate by 4.33x). Constructs two
    symbol-days with known, deliberately different spans (60s and 120s) and
    asserts the derived window is their exact average (90s), not a
    hardcoded constant. Also proves quotes extend the window beyond what
    trades alone would show, since real-world quote updates can straddle
    the very first/last trade."""
    trades = [
        {"symbol": "SYM-A", "date": "2026-01-01", "time": _parse_time("09:00:00.000000"), "price": 100.0, "size": 100},
        {"symbol": "SYM-A", "date": "2026-01-01", "time": _parse_time("09:00:30.000000"), "price": 100.1, "size": 100},
        {"symbol": "SYM-A", "date": "2026-01-01", "time": _parse_time("09:01:00.000000"), "price": 100.2, "size": 100},
        {"symbol": "SYM-B", "date": "2026-01-01", "time": _parse_time("10:00:00.000000"), "price": 50.0, "size": 100},
        {"symbol": "SYM-B", "date": "2026-01-01", "time": _parse_time("10:02:00.000000"), "price": 50.1, "size": 100},
    ]
    quotes = []
    window_trades_only = _compute_window_seconds(trades, quotes)
    assert window_trades_only == 90.0, f"expected (60+120)/2=90.0 from trades alone, got {window_trades_only}"

    # A quote 30s after SYM-A's last trade must widen SYM-A's span to 90s,
    # pulling the two-symbol average up to (90+120)/2=105 -- proves quotes
    # are genuinely folded into the bound, not silently ignored.
    quotes = [
        {"symbol": "SYM-A", "date": "2026-01-01", "time": _parse_time("09:01:30.000000"), "bid": 100.15, "ask": 100.25},
    ]
    window_with_quote = _compute_window_seconds(trades, quotes)
    assert window_with_quote == 105.0, f"expected (90+120)/2=105.0 once the quote widens SYM-A, got {window_with_quote}"

    # End-to-end: compute_all()'s reported rate must match a hand-computed
    # value using the derived window, not a hardcoded session length --
    # (3 SYM-A trades + 2 SYM-B trades + 1 quote) / 2 symbol-days / 105s.
    result = compute_all(trades, quotes)
    expected_rate = (3 + 2 + 1) / 2 / 105.0
    got = result["order_arrival_rate_proxy_per_second"]
    assert got["window_seconds_used"] == 105.0, got
    assert abs(got["value"] - expected_rate) < 1e-9, f"expected {expected_rate}, got {got['value']}"
    # The old bug's divisor (6.5*3600=23400s) would have produced a rate
    # roughly 223x smaller than the correct one for this tiny synthetic
    # window -- confirms the fix isn't accidentally still using it.
    buggy_rate = (3 + 2 + 1) / 2 / (6.5 * 3600)
    assert got["value"] > buggy_rate * 100, "rate is suspiciously close to what the old fixed-session divisor would give"
    print("arrival-rate-window self-test passed: window derived from real timestamps, "
          "quotes correctly widen the bound, end-to-end rate matches by hand")


def run_selftest():
    """Generates synthetic (obviously fake) data in memory -- never writes a
    CSV to disk -- and checks the pipeline produces sane, non-crashing
    output. Proves the script works before any real WRDS export exists."""
    import random

    _run_csv_roundtrip_selftest()
    _run_diagnose_selftest()
    _run_diagnose_corrections_selftest()
    _run_price_jump_filter_selftest()
    _run_arrival_rate_window_selftest()

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
    assert 0.0 <= result["spread"]["implausible_fraction"] <= 1.0, "implausible_fraction must be a valid fraction"
    assert result["spread"]["implausible_fraction"] == 0.0, "this synthetic data's fixed 2-cent spread should never trip the 200bps implausibility flag"
    assert result["intraday_price_volatility"]["aggregate"]["max"] >= result["intraday_price_volatility"]["aggregate"]["p99"], \
        "max must never be below p99 -- would mean the percentile/max computation disagree on the same data"
    assert "FAKE" in result["intraday_price_volatility"]["by_symbol"], "volatility by_symbol breakdown must be present"
    assert result["trade_size"]["aggregate"]["max"] >= result["trade_size"]["aggregate"]["p99"]
    assert result["spread"]["aggregate"]["max"] >= result["spread"]["aggregate"]["p99"]

    print("self-test passed: pipeline runs end-to-end on synthetic data, shape AND value sanity bounds hold")
    print(json.dumps(result, indent=2, default=str)[:500] + "\n...")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trades_csv", nargs="?")
    parser.add_argument("quotes_csv", nargs="?")
    parser.add_argument("--out", default="output/stylized_facts.json")
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--diagnose-trades", metavar="TRADES_CSV",
                         help="characterize zero-size trade rows in TRADES_CSV and exit -- "
                              "see diagnose_zero_size_trades()'s docstring")
    parser.add_argument("--diagnose-corrections", metavar="TRADES_CSV_WITH_TR_CORR",
                         help="crosstab tr_corr/tr_scond against size<=0 in a trades.csv re-pulled to "
                              "include those columns, and exit -- see diagnose_trade_corrections()'s docstring")
    parser.add_argument("--find-price-jumps", metavar="TRADES_CSV",
                         help="print the top consecutive-trade price changes in TRADES_CSV, with full "
                              "before/after context, and exit -- see find_price_jumps()'s docstring")
    args = parser.parse_args()

    if args.selftest:
        run_selftest()
        return

    if args.diagnose_trades:
        diagnose_zero_size_trades(args.diagnose_trades)
        return

    if args.diagnose_corrections:
        diagnose_trade_corrections(args.diagnose_corrections)
        return

    if args.find_price_jumps:
        find_price_jumps(args.find_price_jumps)
        return

    if not args.trades_csv or not args.quotes_csv:
        parser.error("trades_csv and quotes_csv are required unless --selftest/--diagnose-trades/"
                      "--diagnose-corrections/--find-price-jumps is given")

    trades, trades_dropped = read_trades(args.trades_csv)
    quotes, quotes_dropped = read_quotes(args.quotes_csv)
    if not trades or not quotes:
        sys.exit("no rows read from one or both input files -- check the export matches "
                  "PARAMETER_MAPPING.md's schema")

    trades_total = len(trades) + trades_dropped
    quotes_total = len(quotes) + quotes_dropped
    print(f"dropped {trades_dropped}/{trades_total} trade rows with size <= 0 "
          f"({trades_dropped / trades_total:.1%})")
    print(f"dropped {quotes_dropped}/{quotes_total} quote rows with bidsiz/asksiz <= 0 "
          f"({quotes_dropped / quotes_total:.1%})")

    result = compute_all(trades, quotes)

    implausible = result["spread"]["implausible_fraction"]
    if implausible > 0.05:
        print(f"WARNING: {implausible:.1%} of remaining quotes have relative spread over "
              f"{result['spread']['implausible_threshold_bps']:.0f}bps -- see result['spread']"
              "['implausible_note'] in the output, and PARAMETER_MAPPING.md's 'Known spread "
              "pitfall' section, before trusting the spread numbers below.")

    with open(args.out, "w") as f:
        json.dump(result, f, indent=2, default=str)
    print(f"wrote {args.out} ({len(trades)} trades, {len(quotes)} quotes summarized after filtering -- "
          f"no raw rows written)")


if __name__ == "__main__":
    main()
