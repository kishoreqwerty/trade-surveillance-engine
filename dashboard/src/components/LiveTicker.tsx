import { useEffect, useState } from "react";
import { fetchOrderBookSnapshot } from "../api";
import type { DepthSnapshot } from "../types";
import "./LiveTicker.css";

const POLL_INTERVAL_MS = 1500;

// The "live ticker" deliverable: best bid/ask for the selected instrument,
// polled continuously -- a real, changing number (OrderBook::sequence()
// ticks up with every applied event), not a static readout. Distinct from
// OrderBookDepth below, which shows the full depth ladder rather than just
// the inside market.
export default function LiveTicker({ instrumentId }: { instrumentId: string }) {
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

  if (!instrumentId) return <p className="empty-state">Select an instrument to see live quotes.</p>;
  if (error) return <p className="ticker-error">{error}</p>;
  if (!snapshot) return <p className="empty-state">No book state for {instrumentId} yet.</p>;

  const bestBid = snapshot.bids[0];
  const bestAsk = snapshot.asks[0];

  return (
    <div className="live-ticker tabular">
      <span className="ticker-symbol">{snapshot.instrument_id}</span>
      <span className="ticker-side ticker-bid">{bestBid ? bestBid.price.toFixed(4) : "—"}</span>
      <span className="ticker-x">&times;</span>
      <span className="ticker-side ticker-ask">{bestAsk ? bestAsk.price.toFixed(4) : "—"}</span>
      <span className="ticker-seq">seq {snapshot.sequence}</span>
    </div>
  );
}
