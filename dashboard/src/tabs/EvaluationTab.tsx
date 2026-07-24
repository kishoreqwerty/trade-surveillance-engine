import { useEffect, useState } from "react";
import { fetchEvaluation } from "../api";
import type { EvaluationResponse } from "../types";
import DetectorMetricsBars from "../components/DetectorMetricsBars";
import RateSweepChart from "../components/RateSweepChart";
import SeverityGradientChart from "../components/SeverityGradientChart";
import SaraoCallout from "../components/SaraoCallout";
import "./EvaluationTab.css";

function formatDateTime(ns: number): string {
  return new Date(ns / 1e6).toLocaleString(undefined, { hour12: false });
}

// EVALUATION reads three committed, harness-generated JSON snapshots
// (GET /api/evaluation) -- fetched once on mount, deliberately NOT on
// MONITOR/ALERTS/BOOK's 2-3s live poll cadence, since regenerating this
// data costs real minutes against a real Kafka broker (an 11-point rate
// sweep + two full harness replays), not a cheap DB query. The badge
// below is what keeps that distinction visible rather than silently
// implying the same real-time freshness the other three tabs have.
export default function EvaluationTab() {
  const [data, setData] = useState<EvaluationResponse | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    fetchEvaluation()
      .then((d) => {
        if (!cancelled) setData(d);
      })
      .catch((e) => {
        if (!cancelled) setError(e instanceof Error ? e.message : String(e));
      });
    return () => {
      cancelled = true;
    };
  }, []);

  if (error) {
    return (
      <section className="panel">
        <p className="ticker-error">{error}</p>
        <p className="empty-state">
          Regenerate snapshots: tse_harness_eval --json, tse_sarao_validation --json, cpp/harness/run_rate_sweep.sh
        </p>
      </section>
    );
  }
  if (!data) return <p className="empty-state">Loading evaluation snapshot…</p>;

  const { evaluation, sarao, spoofing_rate_sweep } = data;
  const oldestGeneratedAtNs = Math.min(
    evaluation.generated_at_unix_ns,
    sarao.generated_at_unix_ns,
    spoofing_rate_sweep.generated_at_unix_ns,
  );
  const detectorNames = evaluation.severity_gradient.length > 0
    ? Object.keys(evaluation.severity_gradient[0].recall_by_detector)
    : [];

  return (
    <>
      <div className="eval-snapshot-badge">
        HARNESS SNAPSHOT — generated {formatDateTime(oldestGeneratedAtNs)} — not live data, regenerated manually
      </div>

      <div className="eval-grid">
        <section className="panel">
          <div className="panel-header">
            <h2>Precision / Recall / F1 by Detector</h2>
          </div>
          <DetectorMetricsBars detectors={evaluation.detectors} />
        </section>

        <section className="panel">
          <div className="panel-header">
            <h2>Rate Sweep — {spoofing_rate_sweep.detector}</h2>
          </div>
          <RateSweepChart points={spoofing_rate_sweep.points} />
        </section>

        <section className="panel">
          <div className="panel-header">
            <h2>Severity Gradient — Recall vs. Injected Severity</h2>
          </div>
          <SeverityGradientChart
            severities={evaluation.severity_gradient_severities}
            entries={evaluation.severity_gradient}
            detectorNames={detectorNames}
          />
        </section>

        <section className="panel">
          <div className="panel-header">
            <h2>Real-World Validation</h2>
          </div>
          <SaraoCallout sarao={sarao} />
        </section>
      </div>
    </>
  );
}
