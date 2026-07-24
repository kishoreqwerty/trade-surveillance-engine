import { useState } from "react";
import type { Alert, AlertStatus } from "../types";
import { updateAlertStatus } from "../api";
import "./AlertActions.css";

// "critical" (solid filled, --status-critical -- the same red Critical
// severity already uses elsewhere) marks the one action that's hard to
// walk back (escalating a case); "secondary" (bordered/outlined) covers
// every other, lower-stakes transition.
const NEXT_ACTIONS: Record<AlertStatus, { label: string; target: AlertStatus; style: "critical" | "secondary" }[]> = {
  OPEN: [
    { label: "REVIEW", target: "UNDER_REVIEW", style: "secondary" },
    { label: "ESCALATE", target: "ESCALATED", style: "critical" },
    { label: "CLOSE", target: "CLOSED", style: "secondary" },
  ],
  UNDER_REVIEW: [
    { label: "ESCALATE", target: "ESCALATED", style: "critical" },
    { label: "CLOSE", target: "CLOSED", style: "secondary" },
  ],
  ESCALATED: [{ label: "CLOSE", target: "CLOSED", style: "secondary" }],
  CLOSED: [{ label: "REOPEN", target: "OPEN", style: "secondary" }],
};

// The compliance-action control, factored out of AlertRow so AlertDetail
// (ALERTS' right-hand pane) can drive the exact same real PATCH
// .../status round trip instead of a second, drifting implementation.
export default function AlertActions({ alert, onStatusChanged }: { alert: Alert; onStatusChanged: () => void }) {
  const [pending, setPending] = useState(false);
  const [actionError, setActionError] = useState<string | null>(null);

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
    <div>
      <div className="alert-actions">
        {NEXT_ACTIONS[alert.status].map((action) => (
          <button
            key={action.target}
            type="button"
            className={`action-${action.style}`}
            disabled={pending}
            onClick={() => handleAction(action.target)}
          >
            {action.label}
          </button>
        ))}
      </div>
      {actionError && <p className="alert-action-error">{actionError}</p>}
    </div>
  );
}
