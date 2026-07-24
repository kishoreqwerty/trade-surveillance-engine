import { useEffect, useState } from "react";
import { fetchOrderBookSnapshot, fetchStatus } from "../api";
import type { ApiStatus, DepthSnapshot } from "../types";
import "./KpiStrip.css";

const BOOK_POLL_MS = 1500; // matches LiveTicker's own cadence
const STATUS_POLL_MS = 3000; // matches App.tsx's alert poll cadence

// The top KPI strip, visible on every tab (not just MONITOR) -- an
// at-a-glance summary row, same idea as a terminal's persistent header
// ticker. Every value here is a real, currently-polled signal: last/spread
// from the same GET /api/orderbook/.../snapshot LiveTicker already uses,
// the other three from the new GET /api/status (see cpp/api/api_server.cpp
// and its README for what backs each -- none of them are cached-at-startup
// values).
export default function KpiStrip({ instrumentId }: { instrumentId: string }) {
  const [snapshot, setSnapshot] = useState<DepthSnapshot | null>(null);
  const [status, setStatus] = useState<ApiStatus | null>(null);
  const [statusUnreachable, setStatusUnreachable] = useState(false);

  useEffect(() => {
    if (!instrumentId) {
      setSnapshot(null);
      return;
    }
    let cancelled = false;
    async function poll() {
      try {
        const data = await fetchOrderBookSnapshot(instrumentId);
        if (!cancelled) setSnapshot(data);
      } catch {
        // LiveTicker/OrderBookDepth already surface this error prominently
        // elsewhere on MONITOR; the KPI tile just falls back to "--".
      }
    }
    poll();
    const id = setInterval(poll, BOOK_POLL_MS);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, [instrumentId]);

  useEffect(() => {
    let cancelled = false;
    async function poll() {
      try {
        const data = await fetchStatus();
        if (!cancelled) {
          setStatus(data);
          setStatusUnreachable(false);
        }
      } catch {
        if (!cancelled) setStatusUnreachable(true);
      }
    }
    poll();
    const id = setInterval(poll, STATUS_POLL_MS);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, []);

  const bestBid = snapshot?.bids[0];
  const bestAsk = snapshot?.asks[0];
  const spread = bestBid && bestAsk ? bestAsk.price - bestBid.price : null;

  return (
    <div className="kpi-strip">
      <div className="kpi-tile">
        <span className="kpi-label">{instrumentId || "—"} LAST</span>
        <span className="kpi-value tabular">{bestBid ? bestBid.price.toFixed(4) : "—"}</span>
      </div>
      <div className="kpi-tile">
        <span className="kpi-label">SPREAD</span>
        {/* Negative here means a genuinely crossed book, not a display bug
            -- OrderBook (cpp/orderbook/order_book.hpp) deliberately never
            enforces a crossed-book invariant; it's a passive replica of
            externally-reported state, not a matching engine. Flagged in
            the same "bad" red used everywhere else rather than hidden. */}
        <span className={`kpi-value tabular ${spread !== null && spread < 0 ? "kpi-bad" : ""}`}>
          {spread !== null ? spread.toFixed(4) : "—"}
        </span>
      </div>
      <div className="kpi-tile">
        <span className="kpi-label">DETECTORS ACTIVE</span>
        <span className="kpi-value tabular">
          {status && status.detectors_active !== null ? `${status.detectors_active} / ${status.detectors_total}` : "—"}
        </span>
      </div>
      <div className="kpi-tile">
        <span className="kpi-label">DB STATUS</span>
        <span className={`kpi-value ${status?.db_connected ? "kpi-good" : "kpi-bad"}`}>
          {statusUnreachable ? "UNREACHABLE" : status ? (status.db_connected ? "CONNECTED" : "DISCONNECTED") : "—"}
        </span>
      </div>
      <div className="kpi-tile">
        <span className="kpi-label">ML SERVICE</span>
        <span className={`kpi-value ${status?.ml_service_healthy ? "kpi-good" : "kpi-bad"}`}>
          {statusUnreachable ? "—" : status ? (status.ml_service_healthy ? "HEALTHY" : "UNAVAILABLE") : "—"}
        </span>
      </div>
    </div>
  );
}
