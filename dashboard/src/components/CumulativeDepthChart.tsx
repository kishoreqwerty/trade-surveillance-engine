import type { DepthSnapshot, PriceLevel } from "../types";
import "./CumulativeDepthChart.css";

const WIDTH = 640;
const HEIGHT = 220;
const MARGIN = { top: 10, right: 12, bottom: 22, left: 12 };

interface Point {
  price: number;
  cumQty: number;
}

// Cumulative sum walking outward from the best price on each side -- the
// real depth shape, not the mockup's illustrative SVG polyline. Uses the
// FULL book (not OrderBookDepth's capped top-15), since the point of this
// chart is exactly the shape a capped ladder can't show.
function cumulate(levels: PriceLevel[]): Point[] {
  let running = 0;
  return levels.map((level) => {
    running += level.total_qty;
    return { price: level.price, cumQty: running };
  });
}

export default function CumulativeDepthChart({ snapshot }: { snapshot: DepthSnapshot | null }) {
  if (!snapshot || (snapshot.bids.length === 0 && snapshot.asks.length === 0)) {
    return <p className="empty-state">No depth to chart yet.</p>;
  }

  // snapshot.bids is best(highest)-first; reversed so both series plot
  // left-to-right in ascending price order on one shared x-axis.
  const bidPoints = [...cumulate(snapshot.bids)].reverse();
  const askPoints = cumulate(snapshot.asks);

  const allPrices = [...bidPoints, ...askPoints].map((p) => p.price);
  const allQty = [...bidPoints, ...askPoints].map((p) => p.cumQty);
  const minPrice = Math.min(...allPrices);
  const maxPrice = Math.max(...allPrices);
  const maxQty = Math.max(...allQty, 1);
  const priceSpan = Math.max(maxPrice - minPrice, 1e-9);

  const innerWidth = WIDTH - MARGIN.left - MARGIN.right;
  const innerHeight = HEIGHT - MARGIN.top - MARGIN.bottom;

  function x(price: number): number {
    return MARGIN.left + ((price - minPrice) / priceSpan) * innerWidth;
  }
  function y(qty: number): number {
    return MARGIN.top + innerHeight - (qty / maxQty) * innerHeight;
  }

  function pathFor(points: Point[]): string {
    return points.map((p, i) => `${i === 0 ? "M" : "L"}${x(p.price).toFixed(2)},${y(p.cumQty).toFixed(2)}`).join(" ");
  }

  const bestBid = snapshot.bids[0]?.price;
  const bestAsk = snapshot.asks[0]?.price;
  const midX = bestBid !== undefined && bestAsk !== undefined ? x((bestBid + bestAsk) / 2) : null;

  return (
    <svg className="depth-chart" viewBox={`0 0 ${WIDTH} ${HEIGHT}`} role="img" aria-label="Cumulative bid/ask depth">
      {midX !== null && (
        <line x1={midX} x2={midX} y1={MARGIN.top} y2={MARGIN.top + innerHeight} className="depth-chart-mid" />
      )}
      <line
        x1={MARGIN.left}
        x2={MARGIN.left + innerWidth}
        y1={MARGIN.top + innerHeight}
        y2={MARGIN.top + innerHeight}
        className="depth-chart-axis"
      />
      {bidPoints.length > 0 && <path d={pathFor(bidPoints)} className="depth-chart-line depth-chart-bid" />}
      {askPoints.length > 0 && <path d={pathFor(askPoints)} className="depth-chart-line depth-chart-ask" />}
      {bidPoints.map((p) => (
        <circle key={`bid-${p.price}`} cx={x(p.price)} cy={y(p.cumQty)} r={2} className="depth-chart-dot depth-chart-dot-bid">
          <title>{`bid ${p.price.toFixed(4)} — cumulative ${p.cumQty}`}</title>
        </circle>
      ))}
      {askPoints.map((p) => (
        <circle key={`ask-${p.price}`} cx={x(p.price)} cy={y(p.cumQty)} r={2} className="depth-chart-dot depth-chart-dot-ask">
          <title>{`ask ${p.price.toFixed(4)} — cumulative ${p.cumQty}`}</title>
        </circle>
      ))}
      <text x={MARGIN.left} y={HEIGHT - 6} className="depth-chart-label">
        {minPrice.toFixed(4)}
      </text>
      <text x={MARGIN.left + innerWidth} y={HEIGHT - 6} textAnchor="end" className="depth-chart-label">
        {maxPrice.toFixed(4)}
      </text>
    </svg>
  );
}
