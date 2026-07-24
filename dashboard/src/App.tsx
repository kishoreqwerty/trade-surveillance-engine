import { useCallback, useEffect, useState } from "react";
import { fetchAlerts } from "./api";
import type { Alert } from "./types";
import TabNav from "./components/TabNav";
import KpiStrip from "./components/KpiStrip";
import type { Tab } from "./tabs";
import MonitorTab from "./tabs/MonitorTab";
import AlertsTab from "./tabs/AlertsTab";
import BookTab from "./tabs/BookTab";
import EvaluationTab from "./tabs/EvaluationTab";
import "./App.css";

const POLL_INTERVAL_MS = 3000;
// One broad fetch, shared by MONITOR and BOOK -- each filters/sorts this
// same real array client-side rather than driving a second,
// independently-filtered server query, which would otherwise let one
// tab's filter text silently narrow what another tab sees. ALERTS is the
// one exception: it has its own real server-side paginated query instead
// (see AlertsTab.tsx's own header comment for why a recency-bounded
// shared sample isn't safe to build a filter dropdown from).
const POLL_LIMIT = 250;

function App() {
  const [activeTab, setActiveTab] = useState<Tab>("MONITOR");
  const [alerts, setAlerts] = useState<Alert[]>([]);
  const [selectedInstrument, setSelectedInstrument] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [lastUpdated, setLastUpdated] = useState<Date | null>(null);

  const refresh = useCallback(async () => {
    try {
      const data = await fetchAlerts({ limit: POLL_LIMIT });
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
  }, []);

  useEffect(() => {
    refresh();
    const id = setInterval(refresh, POLL_INTERVAL_MS);
    return () => clearInterval(id);
  }, [refresh]);

  return (
    <div className="app-shell">
      <header className="app-header">
        <h1>TSE // Trade Surveillance Terminal</h1>
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

      <TabNav active={activeTab} onChange={setActiveTab} />
      <KpiStrip instrumentId={selectedInstrument} />

      <main>
        {activeTab === "MONITOR" && (
          <MonitorTab
            alerts={alerts}
            selectedInstrument={selectedInstrument}
            onSelectInstrument={setSelectedInstrument}
            onStatusChanged={refresh}
          />
        )}
        {activeTab === "ALERTS" && (
          <AlertsTab onStatusChanged={refresh} onSelectInstrument={setSelectedInstrument} />
        )}
        {activeTab === "BOOK" && (
          <BookTab alerts={alerts} selectedInstrument={selectedInstrument} onSelectInstrument={setSelectedInstrument} />
        )}
        {activeTab === "EVALUATION" && <EvaluationTab />}
      </main>
    </div>
  );
}

export default App;
