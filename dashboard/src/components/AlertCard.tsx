import { useState } from "react";
import type { Alert, AlertStatus } from "../types";
import { updateAlertStatus } from "../api";
import { severityFor, colorForDetector } from "../severity";
import "./AlertCard.css";

function formatNs(ns: number): string {
  return new Date(ns / 1e6).toLocaleString();
}

const NEXT_ACTIONS: Record<AlertStatus, { label: string; target: AlertStatus }[]> = {
  OPEN: [
    { label: "Mark Under Review", target: "UNDER_REVIEW" },
    { label: "Escalate", target: "ESCALATED" },
    { label: "Close", target: "CLOSED" },
  ],
  UNDER_REVIEW: [
    { label: "Escalate", target: "ESCALATED" },
    { label: "Close", target: "CLOSED" },
  ],
  ESCALATED: [{ label: "Close", target: "CLOSED" }],
  CLOSED: [{ label: "Reopen", target: "OPEN" }],
};

interface Props {
  alert: Alert;
  onStatusChanged: () => void;
  onSelectInstrument: (instrumentId: string) => void;
  selected: boolean;
}

export default function AlertCard({ alert, onStatusChanged, onSelectInstrument, selected }: Props) {
  const [pending, setPending] = useState(false);
  const [actionError, setActionError] = useState<string | null>(null);
  const severity = severityFor(alert.score);

  async function handleAction(target: AlertStatus) {
    setPending(true);
    setActionError(null);
    try {
      await updateAlertStatus(alert.alert_id, target);
      onStatusChanged();
    } catch (e) {
      setActionError(e instanceof Error ? e.message : String(e));
    } finally {
      setPending(false);
    }
  }

  return (
    <article className={`alert-card${selected ? " alert-card-selected" : ""}`}>
      <div className="alert-card-top">
        <span className="severity-badge" style={{ color: severity.color }}>
          <span aria-hidden="true">{severity.icon}</span> {severity.label}
        </span>
        <span className="detector-chip" style={{ borderColor: colorForDetector(alert.detector_name) }}>
          <span className="detector-dot" style={{ background: colorForDetector(alert.detector_name) }} />
          {alert.detector_name}
        </span>
        <span className={`status-pill status-${alert.status.toLowerCase()}`}>{alert.status}</span>
      </div>

      <button
        type="button"
        className="alert-instrument-link"
        onClick={() => onSelectInstrument(alert.instrument_id)}
        title="View this instrument's order book"
      >
        {alert.instrument_id}
      </button>
      <span className="tabular alert-score"> score {alert.score.toFixed(2)}</span>

      <p className="alert-evidence">{alert.evidence}</p>

      <dl className="alert-meta">
        <dt>Accounts</dt>
        <dd>{alert.account_ids.join(", ") || "—"}</dd>
        <dt>Orders</dt>
        <dd>{alert.order_ids.join(", ") || "—"}</dd>
        <dt>Window</dt>
        <dd className="tabular">
          {formatNs(alert.window_start_ns)} &rarr; {formatNs(alert.window_end_ns)}
        </dd>
        {alert.model_version && (
          <>
            <dt>Model</dt>
            <dd>{alert.model_version}</dd>
          </>
        )}
      </dl>

      <div className="alert-actions">
        {NEXT_ACTIONS[alert.status].map((action) => (
          <button key={action.target} type="button" disabled={pending} onClick={() => handleAction(action.target)}>
            {action.label}
          </button>
        ))}
      </div>
      {actionError && <p className="alert-action-error">{actionError}</p>}
    </article>
  );
}
