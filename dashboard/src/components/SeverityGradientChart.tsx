import type { SeverityGradientEntry } from "../types";
import { colorForDetector } from "../severity";
import "./SeverityGradientChart.css";

const WIDTH = 560;
const HEIGHT = 200;
const MARGIN = { top: 12, right: 16, bottom: 22, left: 26 };

interface Props {
  severities: number[];
  entries: SeverityGradientEntry[];
  detectorNames: string[];
}

// Recall vs. injected severity, one line per pattern-aware detector.
// Colors come from severity.ts's colorForDetector() -- the same fixed
// categorical slot each detector already uses in MONITOR's alert queue
// and event timeline, so a detector reads as the same color everywhere
// in the app, not just within this one chart.
export default function SeverityGradientChart({ severities, entries, detectorNames }: Props) {
  if (entries.length === 0) return <p className="empty-state">No severity-gradient snapshot data.</p>;

  const innerWidth = WIDTH - MARGIN.left - MARGIN.right;
  const innerHeight = HEIGHT - MARGIN.top - MARGIN.bottom;
  const minSeverity = Math.min(...severities);
  const maxSeverity = Math.max(...severities);
  const severitySpan = Math.max(maxSeverity - minSeverity, 1e-9);

  function x(severity: number): number {
    return MARGIN.left + ((severity - minSeverity) / severitySpan) * innerWidth;
  }
  function y(recall: number): number {
    return MARGIN.top + innerHeight - Math.max(0, Math.min(1, recall)) * innerHeight;
  }

  const sortedEntries = [...entries].sort((a, b) => a.severity - b.severity);

  return (
    <div>
      <div className="severity-legend">
        {detectorNames.map((name) => (
          <span key={name}>
            <span className="legend-swatch" style={{ background: colorForDetector(name) }} /> {name}
          </span>
        ))}
      </div>
      <svg className="severity-chart" viewBox={`0 0 ${WIDTH} ${HEIGHT}`} role="img" aria-label="Recall vs injected severity">
        <line x1={MARGIN.left} x2={MARGIN.left + innerWidth} y1={MARGIN.top + innerHeight} y2={MARGIN.top + innerHeight} className="severity-axis" />
        <line x1={MARGIN.left} x2={MARGIN.left} y1={MARGIN.top} y2={MARGIN.top + innerHeight} className="severity-axis" />
        {detectorNames.map((name) => {
          const path = sortedEntries
            .map((e, i) => `${i === 0 ? "M" : "L"}${x(e.severity).toFixed(2)},${y(e.recall_by_detector[name] ?? 0).toFixed(2)}`)
            .join(" ");
          const color = colorForDetector(name);
          return (
            <g key={name}>
              <path d={path} className="severity-line" style={{ stroke: color }} />
              {sortedEntries.map((e) => (
                <circle
                  key={`${name}-${e.severity}`}
                  cx={x(e.severity)}
                  cy={y(e.recall_by_detector[name] ?? 0)}
                  r={2.5}
                  style={{ fill: color }}
                >
                  <title>{`${name} — severity ${e.severity} — recall ${(e.recall_by_detector[name] ?? 0).toFixed(3)}`}</title>
                </circle>
              ))}
            </g>
          );
        })}
        <text x={MARGIN.left} y={HEIGHT - 6} className="severity-label">
          {minSeverity.toFixed(1)}
        </text>
        <text x={MARGIN.left + innerWidth} y={HEIGHT - 6} textAnchor="end" className="severity-label">
          {maxSeverity.toFixed(1)}
        </text>
        <text x={MARGIN.left - 4} y={MARGIN.top + 4} textAnchor="end" className="severity-label">
          1.0
        </text>
        <text x={MARGIN.left - 4} y={MARGIN.top + innerHeight} textAnchor="end" className="severity-label">
          0
        </text>
      </svg>
    </div>
  );
}
