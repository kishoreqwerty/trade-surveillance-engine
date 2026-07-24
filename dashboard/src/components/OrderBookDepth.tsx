import { useEffect, useState } from "react";
import { fetchOrderBookSnapshot } from "../api";
import type { DepthSnapshot, PriceLevel } from "../types";
import "./OrderBookDepth.css";

const POLL_INTERVAL_MS = 2000;

// A real depth ladder shows the top of book, not the whole book -- capping
// here (rather than letting the panel grow to however many price levels
// happen to be resting) is what keeps this a ladder instead of an
// unbounded list that pushes everything below it off-screen.
const VISIBLE_LEVELS = 15;

function Ladder({
  levels,
  totalLevelCount,
  side,
  maxQty,
}: {
  levels: PriceLevel[];
  totalLevelCount: number;
  side: "bid" | "ask";
  maxQty: number;
}) {
  const hidden = totalLevelCount - levels.length;
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
      {hidden > 0 && <p className="ladder-more">+{hidden} more level{hidden === 1 ? "" : "s"}</p>}
    </div>
  );
}

// The "order book depth visualization" deliverable: a two-sided price
// ladder, bar length proportional to resting quantity at that price level
// -- the standard depth-display convention, one shared quantity scale
// across both sides (not a dual-axis chart: both bars encode the same
// measure, just split left/right by side, per the diverging bid/ask
// convention -- green/red (--bid/--ask) is this app's semantic bid/ask
// pair, distinct from the categorical --series-* ramp used for detector
// identity elsewhere).
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

  const visibleBids = snapshot.bids.slice(0, VISIBLE_LEVELS);
  const visibleAsks = snapshot.asks.slice(0, VISIBLE_LEVELS);
  // Scale is derived from what's actually rendered, not the full book --
  // a deep resting level outside the visible top-15 shouldn't compress
  // every visible bar down to look artificially thin.
  const maxQty = Math.max(0, ...visibleBids.map((l) => l.total_qty), ...visibleAsks.map((l) => l.total_qty));

  return (
    <div className="depth-viz">
      <div className="depth-legend">
        <span>
          <span className="legend-swatch" style={{ background: "var(--bid)" }} /> bids
        </span>
        <span>
          <span className="legend-swatch" style={{ background: "var(--ask)" }} /> asks
        </span>
      </div>
      <div className="depth-columns">
        <Ladder levels={visibleBids} totalLevelCount={snapshot.bids.length} side="bid" maxQty={maxQty} />
        <Ladder levels={visibleAsks} totalLevelCount={snapshot.asks.length} side="ask" maxQty={maxQty} />
      </div>
    </div>
  );
}
