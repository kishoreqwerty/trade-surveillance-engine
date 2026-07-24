// Mirrors cpp/api/json_encode.cpp's wire shapes exactly -- these are the
// only two response shapes the API produces, and every field here maps to
// a real detectors::Alert / orderbook::DepthSnapshot field, not an
// invented one. See cpp/api/README.md for the endpoint contract.

export type AlertStatus = "OPEN" | "UNDER_REVIEW" | "ESCALATED" | "CLOSED";

export interface Alert {
  alert_id: number;
  status: AlertStatus;
  detector_name: string;
  score: number;
  instrument_id: string;
  account_ids: string[];
  order_ids: string[];
  window_start_ns: number;
  window_end_ns: number;
  evidence: string;
  model_version: string | null;
  book_snapshot_sequence: number | null;
}

export interface RestingOrder {
  order_id: string;
  account_id: string;
  qty: number;
}

export interface PriceLevel {
  price: number;
  total_qty: number;
  orders: RestingOrder[];
}

export interface DepthSnapshot {
  instrument_id: string;
  sequence: number;
  last_event_timestamp_ns: number;
  bids: PriceLevel[];
  asks: PriceLevel[];
}

// Mirrors GET /api/status exactly -- detectors_active/total are null when
// no live pipeline is attached to that server process (see
// cpp/api/api_server.cpp), not a placeholder value.
export interface ApiStatus {
  detectors_active: number | null;
  detectors_total: number | null;
  db_connected: boolean;
  ml_service_healthy: boolean;
}

// Mirrors GET /api/orderbook/<id>/events -- BOOK's FIX message feed. Same
// tse::fix::Order/Execution fields the rest of the pipeline already uses;
// see cpp/api/live_book_registry.hpp's BookEvent for the source of truth.
export type BookEventType = "NEW" | "CANCEL" | "REPLACE" | "EXECUTION";
export type BookEventSide = "BUY" | "SELL";

export interface BookEvent {
  timestamp_ns: number;
  instrument_id: string;
  msg_type: BookEventType;
  side: BookEventSide;
  price: number;
  qty: number;
  order_id: string;
  account_id: string;
}

// Mirrors GET /api/evaluation exactly -- three committed, harness-generated
// JSON snapshots merged verbatim (see cpp/api/api_server.cpp). Never
// live-computed: generated_at_unix_ns on each sub-object is what lets
// EvaluationTab show "harness snapshot generated <time>" instead of
// implying the same real-time freshness MONITOR/ALERTS/BOOK have.
export interface DetectorEvalEntry {
  name: string;
  target_pattern: string;
  threshold: number;
  tp: number;
  fp: number;
  fn: number;
  tn: number;
  precision: number;
  recall: number;
  f1: number;
}

export interface SeverityGradientEntry {
  severity: number;
  recall_by_detector: Record<string, number>;
}

export interface EvaluationSnapshot {
  generated_at_unix_ns: number;
  replay_integrity: {
    orders: number;
    executions: number;
    total_events_published: number;
    replayed_from_kafka: number;
    events_processed: number;
    events_skipped_inconsistent: number;
    ring_buffer_dropped: number;
  };
  detectors: DetectorEvalEntry[];
  severity_gradient_severities: number[];
  severity_gradient: SeverityGradientEntry[];
}

export interface SaraoSnapshot {
  generated_at_unix_ns: number;
  fired_count: number;
  total_cancel_opportunities: number;
  max_score: number;
  alert_threshold: number;
  fired_cleanly: boolean;
  replay_integrity: {
    orders: number;
    executions: number;
    published: number;
    replayed: number;
    processed: number;
    skipped_inconsistent: number;
    dropped: number;
  };
}

export interface RateSweepPoint {
  rate_per_sec: number;
  threshold: number;
  tp: number;
  fp: number;
  precision: number;
  recall: number;
}

export interface SpoofingRateSweepSnapshot {
  generated_at_unix_ns: number;
  detector: string;
  points: RateSweepPoint[];
}

export interface EvaluationResponse {
  evaluation: EvaluationSnapshot;
  sarao: SaraoSnapshot;
  spoofing_rate_sweep: SpoofingRateSweepSnapshot;
}
