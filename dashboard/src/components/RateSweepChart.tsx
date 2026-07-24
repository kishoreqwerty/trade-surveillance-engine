import type { RateSweepPoint } from "../types";
import "./RateSweepChart.css";

const WIDTH = 560;
const HEIGHT = 200;
const MARGIN = { top: 12, right: 16, bottom: 22, left: 34 };

// Log-scale x: the 11 real rate points span 3 -> 250.35/sec, an ~83x
// range -- a linear axis would crush the first 8 points into the left
// 3% of the chart. Single series (SpoofingLayeringDetector precision
// only), so no legend box per the dataviz skill's "one series needs no
// legend, the title names it" rule.
export default function RateSweepChart({ points }: { points: RateSweepPoint[] }) {
  if (points.length === 0) return <p className="empty-state">No rate-sweep snapshot data.</p>;

  const sorted = [...points].sort((a, b) => a.rate_per_sec - b.rate_per_sec);
  const logRates = sorted.map((p) => Math.log10(p.rate_per_sec));
  const minLog = Math.min(...logRates);
  const maxLog = Math.max(...logRates);
  const logSpan = Math.max(maxLog - minLog, 1e-9);
  const maxPrecision = Math.max(...sorted.map((p) => p.precision), 0.01);

  const innerWidth = WIDTH - MARGIN.left - MARGIN.right;
  const innerHeight = HEIGHT - MARGIN.top - MARGIN.bottom;

  function x(rate: number): number {
    return MARGIN.left + ((Math.log10(rate) - minLog) / logSpan) * innerWidth;
  }
  function y(precision: number): number {
    return MARGIN.top + innerHeight - (precision / maxPrecision) * innerHeight;
  }

  const path = sorted.map((p, i) => `${i === 0 ? "M" : "L"}${x(p.rate_per_sec).toFixed(2)},${y(p.precision).toFixed(2)}`).join(" ");

  const first = sorted[0];
  const last = sorted[sorted.length - 1];
  const collapseFactor = first.precision > 0 && last.precision > 0 ? first.precision / last.precision : null;

  return (
    <div>
      <svg className="rate-sweep-chart" viewBox={`0 0 ${WIDTH} ${HEIGHT}`} role="img" aria-label="Precision vs order rate">
        <line x1={MARGIN.left} x2={MARGIN.left + innerWidth} y1={MARGIN.top + innerHeight} y2={MARGIN.top + innerHeight} className="rate-sweep-axis" />
        <line x1={MARGIN.left} x2={MARGIN.left} y1={MARGIN.top} y2={MARGIN.top + innerHeight} className="rate-sweep-axis" />
        <path d={path} className="rate-sweep-line" />
        {sorted.map((p) => (
          <circle key={p.rate_per_sec} cx={x(p.rate_per_sec)} cy={y(p.precision)} r={3} className="rate-sweep-dot">
            <title>{`rate=${p.rate_per_sec}/sec — precision ${p.precision.toFixed(4)} (TP=${p.tp}, FP=${p.fp})`}</title>
          </circle>
        ))}
        <text x={MARGIN.left} y={HEIGHT - 6} className="rate-sweep-label">
          {first.rate_per_sec}/sec
        </text>
        <text x={MARGIN.left + innerWidth} y={HEIGHT - 6} textAnchor="end" className="rate-sweep-label">
          {last.rate_per_sec}/sec
        </text>
        <text x={MARGIN.left - 6} y={MARGIN.top + 4} textAnchor="end" className="rate-sweep-label">
          {maxPrecision.toFixed(2)}
        </text>
        <text x={MARGIN.left - 6} y={MARGIN.top + innerHeight} textAnchor="end" className="rate-sweep-label">
          0
        </text>
      </svg>
      {collapseFactor !== null && (
        <p className="rate-sweep-note">
          Relative precision collapse ({first.rate_per_sec}/sec → {last.rate_per_sec}/sec):{" "}
          <strong>{collapseFactor.toFixed(1)}x</strong>
        </p>
      )}
    </div>
  );
}
