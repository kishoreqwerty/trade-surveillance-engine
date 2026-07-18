import { useCallback, useEffect, useState } from "react";
import { fetchAlerts } from "./api";
import type { Alert } from "./types";
import AlertQueue from "./components/AlertQueue";
import LiveTicker from "./components/LiveTicker";
import OrderBookDepth from "./components/OrderBookDepth";
import EventTimeline from "./components/EventTimeline";
import "./App.css";

const POLL_INTERVAL_MS = 3000;

function App() {
  const [alerts, setAlerts] = useState<Alert[]>([]);
  const [detectorFilter, setDetectorFilter] = useState("");
  const [accountFilter, setAccountFilter] = useState("");
  const [selectedInstrument, setSelectedInstrument] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [lastUpdated, setLastUpdated] = useState<Date | null>(null);

  const refresh = useCallback(async () => {
    try {
      const data = await fetchAlerts({
        detectorName: detectorFilter || undefined,
        accountId: accountFilter || undefined,
        limit: 100,
      });
      setAlerts(data);
      setError(null);
      setLastUpdated(new Date());
      // Real-data-derived default, not hardcoded: once alerts arrive and
      // nothing is selected yet, default the order-book panel to the most
      // recent alert's own instrument.
      setSelectedInstrument((current) => current || data[0]?.instrument_id || "");
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }, [detectorFilter, accountFilter]);

  useEffect(() => {
    refresh();
    const id = setInterval(refresh, POLL_INTERVAL_MS);
    return () => clearInterval(id);
  }, [refresh]);

  return (
    <div id="root">
      <header className="app-header">
        <h1>Trade Surveillance Dashboard</h1>
        <div className="app-header-status">
          {error ? (
            <span className="conn-badge conn-bad">API unreachable: {error}</span>
          ) : (
            <span className="conn-badge conn-ok">
              live &middot; updated {lastUpdated ? lastUpdated.toLocaleTimeString() : "–"}
            </span>
          )}
        </div>
      </header>

      <main className="app-grid">
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
            alerts={alerts}
            onStatusChanged={refresh}
            onSelectInstrument={setSelectedInstrument}
            selectedInstrument={selectedInstrument}
          />
        </section>

        <section className="panel panel-book">
          <div className="panel-header">
            <h2>Order Book</h2>
            <input
              className="instrument-input"
              placeholder="instrument id"
              value={selectedInstrument}
              onChange={(e) => setSelectedInstrument(e.target.value)}
            />
          </div>
          <LiveTicker instrumentId={selectedInstrument} />
          <OrderBookDepth instrumentId={selectedInstrument} />
        </section>
      </main>

      <section className="panel panel-timeline">
        <div className="panel-header">
          <h2>Event Timeline</h2>
        </div>
        <EventTimeline alerts={alerts} />
      </section>
    </div>
  );
}

export default App;
