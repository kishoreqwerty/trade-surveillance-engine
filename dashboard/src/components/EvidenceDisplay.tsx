import type { Alert } from "../types";
import { buildEvidenceDisplay } from "../evidence";
import "./EvidenceDisplay.css";

// Replaces the old raw <p>{alert.evidence}</p> dump in both AlertRow's
// inline expansion and AlertDetail's pane -- one shared renderer so the two
// never drift into different treatments of the same data. See evidence.ts
// for the per-detector parsing; this component only renders whatever it's
// given.
export default function EvidenceDisplay({ alert }: { alert: Alert }) {
  const { narrative, metrics } = buildEvidenceDisplay(alert);

  return (
    <>
      <p className="alert-evidence">{narrative}</p>
      {metrics && (
        <dl className="evidence-metrics">
          {metrics.map((metric) => (
            <div className="evidence-metric" key={metric.label} title={metric.raw}>
              <dt className="evidence-metric-label">{metric.label}</dt>
              <dd className="evidence-metric-value">{metric.value}</dd>
            </div>
          ))}
        </dl>
      )}
    </>
  );
}
