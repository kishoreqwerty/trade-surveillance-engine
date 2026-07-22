#pragma once

#include <cstdint>
#include <vector>

// Relative, not bare "types.hpp" like sibling abuse/ headers: this header,
// uniquely among them, is included directly from outside cpp/simulator/
// (cpp/harness/sarao_validation_main.cpp), where cpp/fix/'s types.hpp is
// also named "types.hpp" and can win the include-search-path race
// depending on link order (see cpp/harness/CMakeLists.txt's own comment
// on this exact ambiguity). A relative include resolves against this
// file's own directory first, sidestepping the ambiguity regardless of
// any consumer's link order.
#include "../types.hpp"

namespace tse::simulator {

// The CFTC v. Nav Sarao Futures Limited PLC & Navinder Singh Sarao
// "Layering Algorithm" pattern, replayed as a fixed, documented order
// sequence -- not a randomized synthetic scenario. This is CLAUDE.md's
// one deliberate exception to "synthetic only": public regulatory record,
// not proprietary or licensed data, so it's safe to construct and commit
// directly (unlike calibration/'s WRDS/TAQ inputs).
//
// Sources, by tier -- distinguished deliberately, not collapsed into one
// "cited" bucket, because they carry different evidentiary weight:
//
//   PRIMARY: CFTC Press Release 7156-15, cftc.gov/PressRoom/PressReleases/7156-15
//   ("CFTC Charges U.K. Resident Navinder Singh Sarao and His Company Nav
//   Sarao Futures Limited PLC with Price Manipulation and Spoofing").
//   The underlying complaint itself (No. 1:15-cv-03398, N.D. Ill., filed
//   2015-04-21) exists and is public, but its PDF would not extract
//   cleanly when checked for this file -- everything below cited to
//   "PRIMARY" was verified against the press release specifically, not
//   the complaint text directly.
//
//   SECONDARY: Wikipedia's "Spoofing (finance)" article, as of the date
//   this file was written -- used only for the one specific fact the
//   primary press release doesn't state (per-order lot sizes). Flagged
//   distinctly, not blended in with the primary-sourced facts, so nobody
//   mistakes a secondary source's citation for a verified primary one.
//
// CITED, PRIMARY (used as given):
//   - "four to six exceptionally large sell orders" layered simultaneously,
//     "one price level from the other" -- this scenario uses 5.
//   - Orders kept "at least three or four price levels from the best
//     asking price" (dynamically repriced as the market moved).
//   - "the vast majority of the Layering Algorithm orders were canceled
//     without resulting in any transactions" -- every layer order here is
//     New then Cancel; none fill.
//   - Defendants "often cycled the Layering Algorithm on and off several
//     times during a typical trading day" -- this scenario runs 5 cycles.
//   - On 2010-05-06, the algorithm ran "continuously, for over two hours"
//     immediately before the Flash Crash, applying "close to $200 million
//     ... of persistent downward pressure" -- this scenario's total
//     window is ~2h15m, within the source's own "over two hours."
//
// CITED, SECONDARY (not verified by this project against the primary
// complaint -- treat as a real but lower-confidence citation than the
// PRIMARY items above until independently confirmed):
//   - The "188-and-289-lot spoofing technique" -- both order sizes are
//     used directly, alternating across layers.
//
// ILLUSTRATIVE (not in the cited sources, chosen to be consistent with
// them, not to contradict them):
//   - Exact calendar/clock alignment within 2010-05-06 (the sources give
//     the date and "over two hours before the crash," not a to-the-minute
//     start time).
//   - The exact reference price level (~1150, the E-mini S&P 500's
//     approximate range in that period) and tick-by-tick price path --
//     reconstructing the real one would need the actual CME order/trade
//     tape, which isn't public data this project has or will fabricate.
//   - Per-order dwell time before cancellation (a few seconds each) --
//     the sources establish "rapid," "dynamic" repricing and cite an
//     aggregate ~19,000 modifications across the whole ~5-year, 400+-day
//     pattern (implying short-lived individual instances), but don't give
//     a per-instance duration.
//   - The declining resting-bid path used to give the opposite side
//     something to move against (see .cpp) -- a mechanical stand-in for
//     "persistent downward pressure ... contributed to an extreme E-mini
//     S&P order book imbalance," not a claim about which specific bids
//     existed.
//
// Deliberately does not attempt to reconstruct Sarao's own profit-taking
// trades -- the sources describe the layering mechanics in order-level
// detail but not his own trading in comparable detail, and this scenario
// is scoped to testing whether SpoofingLayeringDetector recognizes the
// layering pattern itself (depth/speed/move/concurrent-layering signals),
// none of which require a reciprocal "genuine" trade to exist.
struct SaraoCaseOutput {
    std::vector<Order> orders;
    std::vector<Execution> executions;
};

// anchor_time_ns: epoch ns the layering window begins at. Defaults to
// 2010-05-06 13:15:00 UTC (see .cpp) -- an illustrative but date-correct
// anchor within the cited "over two hours before the Flash Crash" window.
constexpr int64_t kSaraoDefaultAnchorNs = 1'273'151'700'000'000'000LL;

SaraoCaseOutput build_sarao_case(int64_t anchor_time_ns = kSaraoDefaultAnchorNs);

}  // namespace tse::simulator
