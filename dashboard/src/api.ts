import type { Alert, AlertStatus, ApiStatus, BookEvent, DepthSnapshot, EvaluationResponse } from "./types";

// No hardcoded/mock data anywhere in this module or any component that
// calls it -- every function here is a real fetch() against cpp/api/'s
// running Crow server. VITE_API_BASE_URL lets the dev server point at
// whatever host:port the live demo server (cpp/api_server) is actually
// listening on; defaults to the demo server's own default port
// (cpp/api/main.cpp's http_port default, 8081).
const API_BASE_URL = import.meta.env.VITE_API_BASE_URL ?? "http://127.0.0.1:8081";

async function getJson<T>(path: string): Promise<T> {
  const res = await fetch(`${API_BASE_URL}${path}`);
  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error(`${path} -> ${res.status}: ${body.error ?? res.statusText}`);
  }
  return res.json() as Promise<T>;
}

export interface AlertFilter {
  accountId?: string;
  detectorName?: string;
  startNs?: number;
  endNs?: number;
  limit?: number;
}

export async function fetchAlerts(filter: AlertFilter = {}): Promise<Alert[]> {
  const params = new URLSearchParams();
  if (filter.accountId) params.set("account_id", filter.accountId);
  if (filter.detectorName) params.set("detector_name", filter.detectorName);
  if (filter.startNs !== undefined) params.set("start_ns", String(filter.startNs));
  if (filter.endNs !== undefined) params.set("end_ns", String(filter.endNs));
  if (filter.limit !== undefined) params.set("limit", String(filter.limit));
  const query = params.toString();
  const { alerts } = await getJson<{ alerts: Alert[] }>(`/api/alerts${query ? `?${query}` : ""}`);
  return alerts;
}

export interface AlertPageFilter {
  detectorName?: string;
  status?: AlertStatus;
  offset: number;
  limit: number;
}

export interface AlertPage {
  alerts: Alert[];
  totalCount: number;
}

// ALERTS tab's page-footer query -- real server-side offset/limit plus a
// real total_count (see cpp/db/alert_store.cpp's list_alerts_paginated()),
// distinct from fetchAlerts() above (which every other tab still uses for
// the shared "most recent 250" poll). Free-text search stays client-side
// (see AlertsTab.tsx), so it isn't a param here.
export async function fetchAlertPage(filter: AlertPageFilter): Promise<AlertPage> {
  const params = new URLSearchParams();
  params.set("offset", String(filter.offset));
  params.set("limit", String(filter.limit));
  if (filter.detectorName) params.set("detector_name", filter.detectorName);
  if (filter.status) params.set("status", filter.status);
  const { alerts, total_count } = await getJson<{ alerts: Alert[]; total_count: number }>(
    `/api/alerts?${params.toString()}`,
  );
  return { alerts, totalCount: total_count };
}

export async function fetchAlert(alertId: number): Promise<Alert> {
  return getJson<Alert>(`/api/alerts/${alertId}`);
}

export async function updateAlertStatus(alertId: number, status: AlertStatus): Promise<Alert> {
  const res = await fetch(`${API_BASE_URL}/api/alerts/${alertId}/status`, {
    method: "PATCH",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ status }),
  });
  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error(`PATCH status -> ${res.status}: ${body.error ?? res.statusText}`);
  }
  return res.json() as Promise<Alert>;
}

export async function fetchOrderBookSnapshot(instrumentId: string): Promise<DepthSnapshot | null> {
  const res = await fetch(`${API_BASE_URL}/api/orderbook/${encodeURIComponent(instrumentId)}/snapshot`);
  if (res.status === 404) return null;
  if (!res.ok) {
    const body = await res.json().catch(() => ({}));
    throw new Error(`orderbook snapshot -> ${res.status}: ${body.error ?? res.statusText}`);
  }
  return res.json() as Promise<DepthSnapshot>;
}

export async function fetchStatus(): Promise<ApiStatus> {
  return getJson<ApiStatus>("/api/status");
}

export async function fetchBookEvents(instrumentId: string, limit = 50): Promise<BookEvent[]> {
  const { events } = await getJson<{ events: BookEvent[] }>(
    `/api/orderbook/${encodeURIComponent(instrumentId)}/events?limit=${limit}`,
  );
  return events;
}

export async function fetchEvaluation(): Promise<EvaluationResponse> {
  return getJson<EvaluationResponse>("/api/evaluation");
}
