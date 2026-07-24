import { useState } from "react";
import type { Alert } from "../types";
import { severityFor, colorForDetector } from "../severity";
import AlertActions from "./AlertActions";
import EvidenceDisplay from "./EvidenceDisplay";
import "./AlertRow.css";

// 24h, no AM/PM suffix -- fits the narrow TIME column on one line and
// reads more like a terminal timestamp than a consumer-app one.
function formatTime(ns: number): string {
  return new Date(ns / 1e6).toLocaleTimeString(undefined, { hour12: false });
}
function formatDateTime(ns: number): string {
  return new Date(ns / 1e6).toLocaleString(undefined, { hour12: false });
}

export type AlertSortKey = "time" | "score";

// The dense, single-line alert-row primitive. Deliberately generic (only
// depends on Alert + a handful of callbacks, nothing MONITOR-specific) so
// ALERTS' master-detail list imports AlertRow/AlertRowHeader directly
// rather than reimplementing a similar-but-different row -- this file is
// the one place row layout/columns/severity-coloring logic lives.
//
// Sort props are optional: MONITOR renders <AlertRowHeader /> with none of
// them and gets the original plain, non-interactive header; ALERTS passes
// all three to get clickable, sort-indicating columns. Same header, two
// contexts, no fork.
export function AlertRowHeader({
  sortKey,
  sortDir,
  onSort,
}: {
  sortKey?: AlertSortKey;
  sortDir?: "asc" | "desc";
  onSort?: (key: AlertSortKey) => void;
} = {}) {
  function indicator(key: AlertSortKey) {
    if (sortKey !== key) return "";
    return sortDir === "asc" ? " ▲" : " ▼";
  }
  return (
    <div className="alert-row alert-row-header">
      <span className="col-severity">SEV</span>
      <span className={onSort ? "col-time sortable" : "col-time"} onClick={onSort ? () => onSort("time") : undefined}>
        TIME{indicator("time")}
      </span>
      <span className="col-detector">DETECTOR</span>
      <span className="col-instrument">INSTR</span>
      <span className="col-account">ACCOUNT</span>
      <span className={onSort ? "col-score sortable" : "col-score"} onClick={onSort ? () => onSort("score") : undefined}>
        SCORE{indicator("score")}
      </span>
      <span className="col-status">STATUS</span>
    </div>
  );
}

interface Props {
  alert: Alert;
  onStatusChanged: () => void;
  onSelectInstrument: (instrumentId: string) => void;
  selected?: boolean;
  // When provided, clicking the row calls onSelect(alert) instead of
  // toggling an inline evidence panel -- ALERTS' master-detail list uses
  // this to drive the right-hand detail pane; MONITOR leaves it unset and
  // keeps the original click-to-expand-inline behavior.
  onSelect?: (alert: Alert) => void;
}

export default function AlertRow({ alert, onStatusChanged, onSelectInstrument, selected, onSelect }: Props) {
  const [expanded, setExpanded] = useState(false);
  const severity = severityFor(alert.score);
  const controlled = typeof onSelect === "function";

  function handleRowActivate() {
    if (controlled) onSelect!(alert);
    else setExpanded((v) => !v);
  }

  return (
    <div className={`alert-row-group${selected ? " alert-row-selected" : ""}`}>
      <div
        className="alert-row"
        role="button"
        tabIndex={0}
        aria-expanded={controlled ? undefined : expanded}
        onClick={handleRowActivate}
        onKeyDown={(e) => {
          if (e.key === "Enter" || e.key === " ") {
            e.preventDefault();
            handleRowActivate();
          }
        }}
      >
        <span className="col-severity tabular" style={{ color: severity.color }}>
          <span aria-hidden="true">{severity.icon}</span> {severity.label}
        </span>
        <span className="col-time tabular">{formatTime(alert.window_start_ns)}</span>
        <span className="col-detector" style={{ color: colorForDetector(alert.detector_name) }} title={alert.detector_name}>
          {alert.detector_name}
        </span>
        <button
          type="button"
          className="col-instrument alert-instrument-link"
          onClick={(e) => {
            e.stopPropagation();
            onSelectInstrument(alert.instrument_id);
          }}
          title="View this instrument's order book"
        >
          {alert.instrument_id}
        </button>
        <span className="col-account" title={alert.account_ids.join(", ") || undefined}>
          {alert.account_ids.join(", ") || "—"}
        </span>
        <span className="col-score tabular">{alert.score.toFixed(2)}</span>
        <span className={`col-status status-${alert.status.toLowerCase()}`}>[{alert.status}]</span>
      </div>

      {!controlled && expanded && (
        <div className="alert-row-detail">
          <EvidenceDisplay alert={alert} />
          <dl className="alert-meta">
            <dt>Detector</dt>
            <dd>{alert.detector_name}</dd>
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
          </dl>
          <AlertActions alert={alert} onStatusChanged={onStatusChanged} />
        </div>
      )}
    </div>
  );
}
