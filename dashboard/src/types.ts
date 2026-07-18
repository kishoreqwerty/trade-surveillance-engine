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
