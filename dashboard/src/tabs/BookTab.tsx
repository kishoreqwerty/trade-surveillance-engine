import { useEffect, useMemo, useState } from "react";
import { fetchOrderBookSnapshot } from "../api";
import type { Alert, DepthSnapshot } from "../types";
import OrderBookDepth from "../components/OrderBookDepth";
import CumulativeDepthChart from "../components/CumulativeDepthChart";
import FixMessageFeed from "../components/FixMessageFeed";
import "./BookTab.css";

const SNAPSHOT_POLL_MS = 2000; // matches OrderBookDepth's own cadence

interface Props {
  alerts: Alert[];
  selectedInstrument: string;
  onSelectInstrument: (instrumentId: string) => void;
}

// BOOK: per-instrument depth ladder (reuses OrderBookDepth unchanged) +
// cumulative depth chart + FIX message feed, all real data. The
// instrument picker's options come from instrument_ids actually seen in
// `alerts` (same real-data-derivation MonitorTab/AlertsTab already use for
// their own filters) -- there's no separate "list all instruments"
// endpoint to invent options from.
export default function BookTab({ alerts, selectedInstrument, onSelectInstrument }: Props) {
  const [snapshot, setSnapshot] = useState<DepthSnapshot | null>(null);

  const knownInstruments = useMemo(() => {
    const ids = new Set(alerts.map((a) => a.instrument_id));
    if (selectedInstrument) ids.add(selectedInstrument);
    return Array.from(ids).sort();
  }, [alerts, selectedInstrument]);

  // The cumulative chart needs the FULL snapshot (not OrderBookDepth's own
  // internal, capped-at-15 fetch), so BookTab polls it independently here.
  useEffect(() => {
    if (!selectedInstrument) {
      setSnapshot(null);
      return;
    }
    let cancelled = false;
    async function poll() {
      try {
        const data = await fetchOrderBookSnapshot(selectedInstrument);
        if (!cancelled) setSnapshot(data);
      } catch {
        if (!cancelled) setSnapshot(null);
      }
    }
    poll();
    const id = setInterval(poll, SNAPSHOT_POLL_MS);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, [selectedInstrument]);

  return (
    <div className="book-grid">
      <div className="book-left-col">
        <section className="panel">
          <div className="panel-header">
            <h2>Instrument</h2>
          </div>
          <select
            className="book-instrument-select"
            value={selectedInstrument}
            onChange={(e) => onSelectInstrument(e.target.value)}
          >
            {!selectedInstrument && <option value="">select…</option>}
            {knownInstruments.map((id) => (
              <option key={id} value={id}>
                {id}
              </option>
            ))}
          </select>
        </section>

        <section className="panel book-ladder-panel">
          <div className="panel-header">
            <h2>Depth Ladder</h2>
          </div>
          {selectedInstrument ? (
            <OrderBookDepth instrumentId={selectedInstrument} />
          ) : (
            <p className="empty-state">Select an instrument.</p>
          )}
        </section>
      </div>

      <div className="book-right-col">
        <section className="panel book-chart-panel">
          <div className="panel-header">
            <h2>Cumulative Depth Chart</h2>
          </div>
          {selectedInstrument ? (
            <CumulativeDepthChart snapshot={snapshot} />
          ) : (
            <p className="empty-state">Select an instrument.</p>
          )}
        </section>

        <section className="panel book-feed-panel">
          <div className="panel-header">
            <h2>FIX Message Feed</h2>
          </div>
          <div className="book-feed-scroll">
            <FixMessageFeed instrumentId={selectedInstrument} />
          </div>
        </section>
      </div>
    </div>
  );
}
