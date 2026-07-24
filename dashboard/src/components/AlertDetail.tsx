import type { Alert } from "../types";
import { severityFor, colorForDetector } from "../severity";
import AlertActions from "./AlertActions";
import EvidenceDisplay from "./EvidenceDisplay";
import "./AlertDetail.css";

function formatDateTime(ns: number): string {
  return new Date(ns / 1e6).toLocaleString(undefined, { hour12: false });
}

interface Props {
  alert: Alert | null;
  onStatusChanged: () => void;
  onSelectInstrument: (instrumentId: string) => void;
}

// ALERTS' right-hand detail pane -- the full-evidence view a row's inline
// expansion gives on MONITOR, but as a persistent pane driven by row
// selection instead of a per-row toggle. Reuses .alert-evidence/.alert-meta
// from AlertRow.css and the AlertActions control unchanged, so a status
// change here and a status change from MONITOR's inline row both go
// through the exact same PATCH .../status code path.
export default function AlertDetail({ alert, onStatusChanged, onSelectInstrument }: Props) {
  if (!alert) {
    return <p className="empty-state">Select an alert on the left to view its full evidence.</p>;
  }
  const severity = severityFor(alert.score);

  return (
    <div className="alert-detail">
      <div className="alert-detail-top">
        <span className="tabular" style={{ color: severity.color }}>
          <span aria-hidden="true">{severity.icon}</span> {severity.label}
        </span>
        <span className={`status-${alert.status.toLowerCase()}`}>[{alert.status}]</span>
      </div>

      <h3 style={{ color: colorForDetector(alert.detector_name) }}>{alert.detector_name}</h3>

      <div className="alert-detail-instrument">
        <button
          type="button"
          className="alert-instrument-link"
          onClick={() => onSelectInstrument(alert.instrument_id)}
          title="View this instrument's order book"
        >
          {alert.instrument_id}
        </button>
        <span className="tabular"> score {alert.score.toFixed(2)}</span>
      </div>

      <EvidenceDisplay alert={alert} />

      <dl className="alert-meta">
        <dt>Alert ID</dt>
        <dd className="tabular">{alert.alert_id}</dd>
        <dt>Accounts</dt>
        <dd>{alert.account_ids.join(", ") || "—"}</dd>
        <dt>Orders</dt>
        <dd>{alert.order_ids.join(", ") || "—"}</dd>
        <dt>Window</dt>
        <dd className="tabular">
          {formatDateTime(alert.window_start_ns)} &rarr; {formatDateTime(alert.window_end_ns)}
        </dd>
        {alert.model_version && (
          <>
            <dt>Model</dt>
            <dd>{alert.model_version}</dd>
          </>
        )}
        {alert.book_snapshot_sequence != null && (
          <>
            <dt>Book seq</dt>
            <dd className="tabular">{alert.book_snapshot_sequence}</dd>
          </>
        )}
      </dl>

      <AlertActions alert={alert} onStatusChanged={onStatusChanged} />
    </div>
  );
}
