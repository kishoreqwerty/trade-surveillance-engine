#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "i_detector.hpp"

namespace tse::detectors {

struct MarkingTheCloseConfig {
    // Absolute epoch-ns session close time, per instrument — this detector
    // has no access to an Instrument/session registry (IDetector::evaluate
    // only receives OrderBook/DetectorEvent/AccountRegistry), so the close
    // schedule is supplied directly. An instrument absent from this map is
    // simply never evaluated for this pattern — a missing session boundary
    // isn't an error, it's "nothing to check against."
    std::unordered_map<std::string, int64_t> close_time_ns_by_instrument;

    int64_t window_duration_ns{300'000'000'000LL};  // trailing window before close, e.g. 5 minutes
    double concentration_threshold{0.4};             // account's share of total closing-window volume
    int64_t min_account_qty_threshold{100};          // absolute floor -- avoids firing on trivial size

    // Total window volume (across all accounts) must also clear this floor
    // before any concentration check is even attempted. Without this, the
    // very first trade of an otherwise-empty window is trivially 100% of
    // volume-so-far for both its participants — a share threshold alone
    // can't distinguish "genuinely dominates a busy close" from "happened
    // to trade first." Caught while designing this detector's own test
    // scenarios, not a theoretical concern.
    int64_t min_total_window_qty_threshold{500};
};

// "Concentrated activity near session close" (build guide, Phase 5):
// tracks, per instrument, every Execution whose timestamp falls in
// [close_time - window_duration, close_time], accumulating total window
// volume and each participating account's own volume within it. An
// account fires once its window volume clears min_account_qty_threshold
// (an absolute size floor, so a single odd-lot trade near the close on an
// otherwise-quiet instrument can't trivially cross a *percentage*
// threshold) **and** its share of total window volume clears
// concentration_threshold — i.e., it's not just active near the close, it
// *dominates* the close.
//
// Reacts only to the Execution arm of DetectorEvent: "activity" here means
// executed volume, not resting orders — an account could rest a huge order
// near the close that never trades, which isn't marking-the-close (there's
// no price impact without an actual print).
//
// Both sides of a matched trade have their window volume credited — a
// trade's counterparty is just as "active near the close" for that
// quantity as the account that initiated it. Total window volume is
// counted once per Execution (not doubled), so an account's share can
// legitimately exceed 50% if it's the dominant counterparty across most of
// the window's prints.
//
// Alerts once per (instrument, account-GROUP) per detector lifetime, not
// once per qualifying Execution — an account that's already cleared both
// bars doesn't need to keep re-alerting as it keeps trading through the
// same close.
//
// Phase 11.5: concentration is checked against an account's beneficial-
// owner/linked-account GROUP, not the raw account_id alone — reusing
// AccountRegistry::is_related() (the exact relation WashTradeDetector
// already uses), via an incremental union-find discovered as new accounts
// are first seen. This closes a real evasion path a per-account-only check
// left open: splitting a marking-the-close scheme's volume across 2-3
// related accounts kept each individual account's own share under
// concentration_threshold even when their COMBINED share obviously
// dominated the close. See PARAMETER_MAPPING.md for the investigation that
// found and fixed this (a per-account rate sweep first exposed it as an
// apparent detector limitation before this fix).
class MarkingTheCloseDetector : public IDetector {
public:
    explicit MarkingTheCloseDetector(MarkingTheCloseConfig config);

    std::vector<Alert> evaluate(const tse::orderbook::OrderBook& book, const DetectorEvent& incoming,
                                 const AccountRegistry& accounts) override;

    std::string name() const override { return "MarkingTheCloseDetector"; }

private:
    std::vector<Alert> handle_execution(const tse::fix::Execution& execution, int64_t book_snapshot_sequence,
                                         const AccountRegistry& accounts);
    std::optional<Alert> check_account(const std::string& instrument_id, const std::string& account_id,
                                        int64_t window_start_ns, int64_t event_ts, int64_t book_snapshot_sequence,
                                        const AccountRegistry& accounts);

    // Registers account_id (if not already known) and unions its group with
    // every previously-seen account it's is_related() to. O(accounts seen
    // so far) per NEW account, not per execution -- cheap at this project's
    // account-pool scale, and only ever runs once per distinct account_id.
    void register_and_group(const std::string& account_id, const AccountRegistry& accounts);
    // Path-compressed union-find lookup. Deliberately NOT used as a storage
    // key anywhere (see .cpp) -- representatives can change identity across
    // a later merge, so anything stored under one would silently go stale.
    std::string find_group(const std::string& account_id);
    // Every raw account_id currently in the same group as account_id,
    // freshly resolved via find_group() -- never cached, so a merge
    // discovered after some volume/alert state already exists is picked up
    // correctly on the next check, not silently missed.
    std::vector<std::string> group_members(const std::string& account_id);

    MarkingTheCloseConfig config_;
    std::unordered_map<std::string, int64_t> total_window_qty_;    // key: instrument_id
    std::unordered_map<std::string, int64_t> account_window_qty_;  // key: instrument_id + "|" + account_id
    std::unordered_set<std::string> alerted_;                      // same key shape as account_window_qty_

    // Every trade_id that contributed to a (instrument, account)'s window
    // volume so far, same key shape as account_window_qty_ -- what
    // check_account() populates Alert::order_ids from. Without this, a
    // fired Alert carried instrument/account/window/score but no reference
    // to which actual executions constitute its evidence, violating the
    // architecture doc's evidence contract (alert.hpp: "order/trade IDs
    // that constitute the evidence") -- the one detector of the five that
    // didn't set this field, found by cpp/harness/'s Phase 10 evaluation
    // (which scores Alerts purely off Alert::order_ids and got a
    // structural 0 TP for this detector until this was added).
    std::unordered_map<std::string, std::vector<std::string>> trade_ids_by_key_;

    // Union-find over account_id, global (not instrument-scoped) since
    // relatedness is a property of the accounts themselves, not of a
    // specific instrument's window.
    std::unordered_map<std::string, std::string> group_parent_;
    std::unordered_set<std::string> all_accounts_seen_;
};

}  // namespace tse::detectors
