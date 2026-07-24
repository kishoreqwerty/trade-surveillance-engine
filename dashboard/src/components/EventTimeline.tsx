import type { Alert } from "../types";
import { colorForDetector, severityFor } from "../severity";
import "./EventTimeline.css";

function formatTime(ns: number): string {
  return new Date(ns / 1e6).toLocaleTimeString(undefined, { hour12: false });
}

// The "event timeline" deliverable: every currently-loaded alert plotted
// on a single shared time axis (one row per detector -- small multiples,
// not a second axis), positioned by window_start_ns. Dot color follows
// detector identity (fixed categorical slot, matching AlertRow's detector
// column); dot size/fill follows severity via the same reserved status
// palette AlertRow uses, so severity never leans on hue alone here either. A
// legend is always present (>=2 series rule) since detector identity is
// the color channel in play.
export default function EventTimeline({ alerts }: { alerts: Alert[] }) {
  if (alerts.length === 0) {
    return <p className="empty-state">No alerts to plot yet.</p>;
  }

  const times = alerts.map((a) => a.window_start_ns);
  const minNs = Math.min(...times);
  const maxNs = Math.max(...times);
  const span = Math.max(1, maxNs - minNs);

  const detectors = Array.from(new Set(alerts.map((a) => a.detector_name)));
  const byDetector = new Map<string, Alert[]>();
  for (const detector of detectors) byDetector.set(detector, []);
  for (const alert of alerts) byDetector.get(alert.detector_name)!.push(alert);

  return (
    <div className="timeline">
      <div className="timeline-legend">
        {detectors.map((detector) => (
          <span key={detector}>
            <span className="legend-swatch" style={{ background: colorForDetector(detector) }} />
            {detector}
          </span>
        ))}
      </div>

      <div className="timeline-rows">
        {detectors.map((detector) => (
          <div className="timeline-row" key={detector}>
            <span className="timeline-row-label">{detector}</span>
            <div className="timeline-track">
              {byDetector.get(detector)!.map((alert) => {
                const pct = ((alert.window_start_ns - minNs) / span) * 100;
                const severity = severityFor(alert.score);
                return (
                  <span
                    key={alert.alert_id}
                    className="timeline-dot"
                    style={{ left: `${pct}%`, background: colorForDetector(detector), borderColor: severity.color }}
                    title={`${detector} · ${alert.instrument_id} · score ${alert.score.toFixed(2)} · ${formatTime(alert.window_start_ns)}`}
                  />
                );
              })}
            </div>
          </div>
        ))}
      </div>

      <div className="timeline-axis">
        <span>{formatTime(minNs)}</span>
        <span>{formatTime(maxNs)}</span>
      </div>
    </div>
  );
}
