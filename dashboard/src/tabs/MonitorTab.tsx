import { useMemo, useState } from "react";
import type { Alert } from "../types";
import AlertQueue from "../components/AlertQueue";
import LiveTicker from "../components/LiveTicker";
import OrderBookDepth from "../components/OrderBookDepth";
import EventTimeline from "../components/EventTimeline";

interface Props {
  alerts: Alert[];
  selectedInstrument: string;
  onSelectInstrument: (instrumentId: string) => void;
  onStatusChanged: () => void;
}

// Phase 12's MONITOR tab: the combined overview -- everything Phase 9's
// single-page dashboard used to show, now living under its own tab.
// Filter text here is applied client-side to the same real `alerts` array
// App.tsx polls (not a second server query) -- see App.tsx's POLL_LIMIT
// comment for why that's a deliberate change from Phase 9's original
// server-side-filter behavior.
export default function MonitorTab({ alerts, selectedInstrument, onSelectInstrument, onStatusChanged }: Props) {
  const [detectorFilter, setDetectorFilter] = useState("");
  const [accountFilter, setAccountFilter] = useState("");

  const filteredAlerts = useMemo(() => {
    const detectorNeedle = detectorFilter.trim().toLowerCase();
    const accountNeedle = accountFilter.trim().toLowerCase();
    return alerts.filter((a) => {
      if (detectorNeedle && !a.detector_name.toLowerCase().includes(detectorNeedle)) return false;
      if (accountNeedle && !a.account_ids.some((id) => id.toLowerCase().includes(accountNeedle))) return false;
      return true;
    });
  }, [alerts, detectorFilter, accountFilter]);

  return (
    <>
      <div className="app-grid">
        <section className="panel panel-queue">
          <div className="panel-header">
            <h2>Alert Queue</h2>
            <div className="filters">
              <input
                placeholder="filter: detector name"
                value={detectorFilter}
                onChange={(e) => setDetectorFilter(e.target.value)}
              />
              <input
                placeholder="filter: account id"
                value={accountFilter}
                onChange={(e) => setAccountFilter(e.target.value)}
              />
            </div>
          </div>
          <AlertQueue
            alerts={filteredAlerts}
            onStatusChanged={onStatusChanged}
            onSelectInstrument={onSelectInstrument}
            selectedInstrument={selectedInstrument}
          />
        </section>

        <div className="monitor-right-col">
          <section className="panel panel-book">
            <div className="panel-header">
              <h2>Order Book</h2>
              <input
                className="instrument-input"
                placeholder="instrument id"
                value={selectedInstrument}
                onChange={(e) => onSelectInstrument(e.target.value)}
              />
            </div>
            <LiveTicker instrumentId={selectedInstrument} />
            <OrderBookDepth instrumentId={selectedInstrument} />
          </section>

          <section className="panel panel-timeline">
            <div className="panel-header">
              <h2>Event Timeline</h2>
            </div>
            <EventTimeline alerts={filteredAlerts} />
          </section>
        </div>
      </div>
    </>
  );
}
