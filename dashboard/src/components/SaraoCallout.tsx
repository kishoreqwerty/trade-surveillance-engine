import type { SaraoSnapshot } from "../types";
import "./SaraoCallout.css";

// The one real-world-verified detection this project has: the documented
// CFTC v. Nav Sarao Futures Limited PLC & Navinder Singh Sarao Layering
// Algorithm pattern, replayed through the actual live pipeline (not a
// synthetic scenario). Citation matches cpp/simulator/abuse/sarao_case.hpp
// verbatim -- the primary source, not the earlier unverified secondary
// source this project's own history flagged and corrected.
export default function SaraoCallout({ sarao }: { sarao: SaraoSnapshot }) {
  return (
    <div className={`sarao-callout${sarao.fired_cleanly ? " sarao-fired" : " sarao-not-fired"}`}>
      <div className="sarao-title">CFTC v. Nav Sarao Futures Limited PLC &amp; Navinder Singh Sarao</div>
      <div className="sarao-citation">
        CFTC Press Release 7156-15 — "CFTC Charges U.K. Resident Navinder Singh Sarao and His Company Nav Sarao
        Futures Limited PLC with Price Manipulation and Spoofing"
      </div>

      <div className="sarao-result">
        <span className={sarao.fired_cleanly ? "sarao-status-good" : "sarao-status-bad"}>
          {sarao.fired_cleanly ? "FIRED" : "DID NOT FIRE"}
        </span>{" "}
        <span className="tabular">
          {sarao.fired_count} / {sarao.total_cancel_opportunities}
        </span>{" "}
        cancel opportunities in the documented Layering Algorithm pattern
      </div>

      <dl className="sarao-meta">
        <dt>Max score</dt>
        <dd className="tabular">{sarao.max_score.toFixed(4)}</dd>
        <dt>Alert threshold</dt>
        <dd className="tabular">{sarao.alert_threshold.toFixed(1)}</dd>
        <dt>Detector</dt>
        <dd>SpoofingLayeringDetector</dd>
      </dl>
    </div>
  );
}
