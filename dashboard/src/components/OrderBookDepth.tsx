import { useEffect, useState } from "react";
import { fetchOrderBookSnapshot } from "../api";
import type { DepthSnapshot, PriceLevel } from "../types";
import "./OrderBookDepth.css";

const POLL_INTERVAL_MS = 2000;

function Ladder({ levels, side, maxQty }: { levels: PriceLevel[]; side: "bid" | "ask"; maxQty: number }) {
  return (
    <div className={`ladder ladder-${side}`}>
      {levels.length === 0 && <p className="empty-state">no {side}s</p>}
      {levels.map((level) => {
        const widthPct = maxQty > 0 ? Math.max(4, (level.total_qty / maxQty) * 100) : 0;
        return (
          <div className="ladder-row" key={level.price} title={`${level.orders.length} order(s), qty ${level.total_qty}`}>
            {side === "ask" && <span className="tabular ladder-qty">{level.total_qty}</span>}
            <div className="ladder-bar-track">
              <div className={`ladder-bar ladder-bar-${side}`} style={{ width: `${widthPct}%` }} />
            </div>
            <span className="tabular ladder-price">{level.price.toFixed(4)}</span>
            {side === "bid" && <span className="tabular ladder-qty">{level.total_qty}</span>}
          </div>
        );
      })}
    </div>
  );
}

// The "order book depth visualization" deliverable: a two-sided price
// ladder, bar length proportional to resting quantity at that price level
// -- the standard depth-display convention, one shared quantity scale
// across both sides (not a dual-axis chart: both bars encode the same
// measure, just split left/right by side, per the diverging bid/ask
// convention -- blue/red is this app's diverging pair, per the dataviz
// skill's palette).
export default function OrderBookDepth({ instrumentId }: { instrumentId: string }) {
  const [snapshot, setSnapshot] = useState<DepthSnapshot | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (!instrumentId) {
      setSnapshot(null);
      return;
    }
    let cancelled = false;
    async function poll() {
      try {
        const data = await fetchOrderBookSnapshot(instrumentId);
        if (!cancelled) {
          setSnapshot(data);
          setError(null);
        }
      } catch (e) {
        if (!cancelled) setError(e instanceof Error ? e.message : String(e));
      }
    }
    poll();
    const id = setInterval(poll, POLL_INTERVAL_MS);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, [instrumentId]);

  if (!instrumentId || error || !snapshot) return null; // LiveTicker above already surfaces the relevant message

  const maxQty = Math.max(
    0,
    ...snapshot.bids.map((l) => l.total_qty),
    ...snapshot.asks.map((l) => l.total_qty),
  );

  return (
    <div className="depth-viz">
      <div className="depth-legend">
        <span>
          <span className="legend-swatch" style={{ background: "var(--series-1)" }} /> bids
        </span>
        <span>
          <span className="legend-swatch" style={{ background: "var(--series-8)" }} /> asks
        </span>
      </div>
      <div className="depth-columns">
        <Ladder levels={snapshot.bids} side="bid" maxQty={maxQty} />
        <Ladder levels={snapshot.asks} side="ask" maxQty={maxQty} />
      </div>
    </div>
  );
}
