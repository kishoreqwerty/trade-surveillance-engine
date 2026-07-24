import type { Alert } from "./types";

// Turns each detector's Alert::evidence free-text string (see
// cpp/detectors/*.cpp and cpp/ml_client/ml_scoring_worker.cpp) into a short
// compliance-readable narrative plus, where the raw evidence carries
// numeric detail worth surfacing on its own, a small labeled metrics grid.
// Display-only: nothing here changes what data exists, only how it reads.
//
// Each parser matches its own detector's exact, stable C++ format string --
// this project owns both sides of that string, so a precise regex per
// detector is safe, not fragile against arbitrary third-party text. If a
// format ever drifts (or the evidence is otherwise malformed), the parser
// returns null and the caller falls back to the original raw evidence
// string -- never a blank or broken narrative.

export interface EvidenceMetric {
  label: string;
  value: string;
  // Full-precision source value, shown via a title="" tooltip. Omitted
  // when `value` already *is* the full-precision value (integers, IDs).
  raw?: string;
}

export interface EvidenceDisplay {
  narrative: string;
  metrics: EvidenceMetric[] | null;
}

function formatDuration(ns: number): string {
  if (ns < 1_000) return `${ns}ns`;
  if (ns < 1_000_000) return `${(ns / 1_000).toFixed(2)}µs`;
  if (ns < 1_000_000_000) return `${(ns / 1_000_000).toFixed(2)}ms`;
  return `${(ns / 1_000_000_000).toFixed(2)}s`;
}

function formatPercent(ratio: number, decimals = 1): string {
  const rounded = (ratio * 100).toFixed(decimals);
  return `${rounded.replace(/\.0+$/, "")}%`;
}

function formatNum(n: number, decimals = 1): string {
  return n.toFixed(decimals).replace(/\.0+$/, "");
}

function formatInt(text: string): string {
  return parseInt(text, 10).toLocaleString();
}

function parseSpoofingLayering(evidence: string): EvidenceDisplay | null {
  const m = evidence.match(
    /^depth_ratio=([\d.]+) speed=([\d.]+) opposite_price_moved_favorably=(true|false) move_score=([\d.]+) concurrent_same_side_orders=(\d+) typical_concurrent=([\d.]+) layering_score=([\d.]+) time_in_book_ns=(\d+)$/
  );
  if (!m) return null;
  const [, depthRatio, speed, movedFavorably, moveScore, concurrent, typicalConcurrent, layeringScore, timeInBookNs] = m;
  const depth = parseFloat(depthRatio);
  const moved = movedFavorably === "true";
  const move = parseFloat(moveScore);
  const typical = parseFloat(typicalConcurrent);
  const layering = parseFloat(layeringScore);
  const spd = parseFloat(speed);
  const dwellNs = parseInt(timeInBookNs, 10);

  const movedClause = moved ? "opposite-side price moved favorably" : "opposite-side price did not move favorably";

  return {
    narrative:
      `Placed at ${formatPercent(depth)} of visible depth, cancelled after ${formatDuration(dwellNs)}, ` +
      `${movedClause}. ${concurrent} concurrent same-side orders vs. typical ${formatNum(typical)} for this instrument.`,
    metrics: [
      { label: "Depth ratio", value: formatPercent(depth), raw: depthRatio },
      { label: "Time in book", value: formatDuration(dwellNs), raw: `${timeInBookNs}ns` },
      { label: "Concurrent same-side orders", value: concurrent },
      { label: "Typical concurrent", value: formatNum(typical), raw: typicalConcurrent },
      { label: "Move score", value: formatPercent(move), raw: moveScore },
      { label: "Layering score", value: formatPercent(layering), raw: layeringScore },
      { label: "Speed score", value: formatPercent(spd), raw: speed },
    ],
  };
}

// concentration_threshold's default (cpp/detectors/marking_the_close_detector.hpp,
// MarkingTheCloseConfig) -- never overridden by cpp/api/main.cpp's live
// wiring or cpp/harness/replay_runner.cpp's evaluation wiring, so this is
// the real, current threshold everywhere this detector actually runs, not
// an approximation. Alert::evidence has no field carrying it (it's a
// config value, not a per-alert measurement), so display-only duplication
// here is the only way to name it -- keep in sync if that default changes.
const MTC_CONCENTRATION_THRESHOLD = 0.4;

function parseMarkingTheClose(evidence: string): EvidenceDisplay | null {
  const m = evidence.match(/^account-group closing-window volume=(\d+) \/ total=(\d+) \(share=([\d.]+), group_size=(\d+)\)$/);
  if (!m) return null;
  const [, groupQty, totalQty, share, groupSize] = m;
  const shareRatio = parseFloat(share);
  const accountsLabel = groupSize === "1" ? "account" : "accounts";

  return {
    narrative:
      `Linked account group (${groupSize} ${accountsLabel}) executed ${formatInt(groupQty)} of ${formatInt(totalQty)} ` +
      `total closing-window volume — ${formatPercent(shareRatio)} of the window — a concentrated share, above ` +
      `the ${formatPercent(MTC_CONCENTRATION_THRESHOLD, 0)} concentration threshold.`,
    metrics: [
      { label: "Group volume", value: formatInt(groupQty) },
      { label: "Total window volume", value: formatInt(totalQty) },
      { label: "Share of volume", value: formatPercent(shareRatio), raw: share },
      { label: "Group size", value: groupSize },
      { label: "Concentration threshold", value: formatPercent(MTC_CONCENTRATION_THRESHOLD, 0) },
    ],
  };
}

function parseStatisticalBaseline(evidence: string): EvidenceDisplay | null {
  const m = evidence.match(/^order qty=(\d+) z=([\d.-]+) against running mean=([\d.]+) stddev=([\d.]+) \(n=(\d+)\)$/);
  if (!m) return null;
  const [, qty, z, mean, stddev, n] = m;

  return {
    narrative:
      `Order quantity ${formatInt(qty)} is ${formatNum(parseFloat(z), 2)}σ from this account/instrument's ` +
      `running average (${formatNum(parseFloat(mean))}, based on ${n} prior orders) — a statistical outlier ` +
      `against its own recent baseline.`,
    metrics: [
      { label: "Order qty", value: formatInt(qty) },
      { label: "Z-score", value: formatNum(parseFloat(z), 2), raw: z },
      { label: "Running mean qty", value: formatNum(parseFloat(mean)), raw: mean },
      { label: "Std deviation", value: formatNum(parseFloat(stddev)), raw: stddev },
      { label: "Sample size (n)", value: n },
    ],
  };
}

function parseMlAnomaly(evidence: string): EvidenceDisplay | null {
  const m = evidence.match(/^Isolation Forest anomaly_score=([\d.]+) model_version=(.+)$/);
  if (!m) return null;
  const [, score, modelVersion] = m;

  return {
    narrative:
      `Order-flow pattern for this account/instrument window scored ${formatPercent(parseFloat(score))} anomalous ` +
      `by the Isolation Forest model — an unsupervised outlier relative to typical order-flow shape, not a ` +
      `specific rule violation.`,
    metrics: [{ label: "Model version", value: modelVersion }],
  };
}

// Light touch only, not a full narrative/grid rewrite -- FrontRunningDetector's
// evidence is already clean prose (see cpp/detectors/front_running_detector.cpp);
// the one raw nanosecond gap it embeds is the only thing worth fixing here.
function humanizeFrontRunningDuration(evidence: string): string {
  return evidence.replace(/(\d+)ns after/, (_match, nsStr: string) => `${formatDuration(parseInt(nsStr, 10))} after`);
}

export function buildEvidenceDisplay(alert: Alert): EvidenceDisplay {
  switch (alert.detector_name) {
    case "SpoofingLayeringDetector":
      return parseSpoofingLayering(alert.evidence) ?? { narrative: alert.evidence, metrics: null };
    case "MarkingTheCloseDetector":
      return parseMarkingTheClose(alert.evidence) ?? { narrative: alert.evidence, metrics: null };
    case "StatisticalBaselineDetector":
      return parseStatisticalBaseline(alert.evidence) ?? { narrative: alert.evidence, metrics: null };
    case "MlAnomalyDetector":
      return parseMlAnomaly(alert.evidence) ?? { narrative: alert.evidence, metrics: null };
    case "FrontRunningDetector":
      return { narrative: humanizeFrontRunningDuration(alert.evidence), metrics: null };
    default:
      // WashTradeDetector already reads as clean prose, and any future/
      // unknown detector_name has nothing this module knows how to
      // improve on -- show the real evidence text as-is, unmodified.
      return { narrative: alert.evidence, metrics: null };
  }
}
