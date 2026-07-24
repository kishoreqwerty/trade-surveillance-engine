import type { DetectorEvalEntry } from "../types";
import "./DetectorMetricsBars.css";

// Fixed categorical order for the three metrics -- never reassigned per
// row, per the dataviz skill's "assign categorical hues in fixed order"
// rule. StatisticalBaselineDetector doesn't get these colors at all (see
// below): it's a naive-baseline comparison, not a sixth peer detector.
const METRICS: Array<{ key: "precision" | "recall" | "f1"; label: string; color: string }> = [
  { key: "precision", label: "P", color: "var(--series-1)" },
  { key: "recall", label: "R", color: "var(--series-5)" },
  { key: "f1", label: "F1", color: "var(--series-4)" },
];

function Bar({ label, value, color }: { label: string; value: number; color: string }) {
  const pct = Math.max(0, Math.min(1, value)) * 100;
  return (
    <div className="metric-bar-row">
      <span className="metric-bar-label">{label}</span>
      <div className="metric-bar-track">
        <div className="metric-bar-fill" style={{ width: `${pct}%`, background: color }} />
      </div>
      <span className="metric-bar-value tabular">{value.toFixed(3)}</span>
    </div>
  );
}

export default function DetectorMetricsBars({ detectors }: { detectors: DetectorEvalEntry[] }) {
  return (
    <div className="metrics-bars">
      <div className="metrics-legend">
        {METRICS.map((m) => (
          <span key={m.key}>
            <span className="legend-swatch" style={{ background: m.color }} /> {m.label}
          </span>
        ))}
      </div>
      {detectors.map((d) => {
        const isBaseline = d.name === "StatisticalBaselineDetector";
        return (
          <div key={d.name} className={`metrics-detector-block${isBaseline ? " metrics-baseline" : ""}`}>
            <div className="metrics-detector-header">
              <span className="metrics-detector-name">{d.name}</span>
              {isBaseline && <span className="metrics-baseline-tag">naive baseline, not a peer detector</span>}
              <span className="metrics-detector-threshold tabular">@ threshold {d.threshold.toFixed(1)}</span>
            </div>
            {METRICS.map((m) => (
              <Bar key={m.key} label={m.label} value={d[m.key]} color={isBaseline ? "var(--text-muted)" : m.color} />
            ))}
          </div>
        );
      })}
    </div>
  );
}
