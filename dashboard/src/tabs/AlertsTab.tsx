import { useCallback, useEffect, useMemo, useState } from "react";
import type { Alert, AlertStatus } from "../types";
import { fetchAlert, fetchAlertPage } from "../api";
import { DETECTOR_NAMES } from "../detectorNames";
import AlertRow, { AlertRowHeader, type AlertSortKey } from "../components/AlertRow";
import AlertDetail from "../components/AlertDetail";
import "./AlertsTab.css";

const STATUS_OPTIONS: Array<AlertStatus | "ALL"> = ["ALL", "OPEN", "UNDER_REVIEW", "ESCALATED", "CLOSED"];
const PAGE_SIZE = 50;

interface Props {
  onStatusChanged: () => void;
  onSelectInstrument: (instrumentId: string) => void;
}

// ALERTS: the full-width, sortable/filterable master-detail view.
//
// The list panel (left) and the detail panel (right) are two genuinely
// independent data sources, on purpose:
//   - The list is its own paginated GET /api/alerts?offset=...&limit=...
//     fetch (see api.ts's fetchAlertPage), re-run whenever the page number
//     or a server-side filter (detector/status) changes. detector/status
//     are real WHERE-clause filters; free-text search stays client-side,
//     scoped to whatever page is currently loaded -- full correctness
//     there would need an ILIKE search across instrument_id and an
//     unnested account_ids array server-side, real scope beyond
//     pagination itself.
//   - The detail panel fetches its selected alert by id (fetchAlert(),
//     GET /api/alerts/<id>) independently of the list's pagination state
//     entirely. This is deliberate: paging the list must never clear or
//     change what's shown in the detail panel -- an analyst can keep an
//     alert's evidence open while browsing other pages. Changing a
//     *filter* does clear the selection (the previously-selected alert
//     may no longer be relevant to what's being looked at); changing
//     *page* does not.
//
// Nothing in this tab reads App.tsx's shared "most recent 250" poll --
// unlike MONITOR/BOOK, ALERTS has no dependency on it at all (see
// DETECTOR_NAMES above for why the filter dropdown doesn't need it
// either).
export default function AlertsTab({ onStatusChanged, onSelectInstrument }: Props) {
  const [detectorFilter, setDetectorFilter] = useState("ALL");
  const [statusFilter, setStatusFilter] = useState<AlertStatus | "ALL">("ALL");
  const [search, setSearch] = useState("");
  const [sortKey, setSortKey] = useState<AlertSortKey>("time");
  const [sortDir, setSortDir] = useState<"asc" | "desc">("desc");
  const [selectedAlertId, setSelectedAlertId] = useState<number | null>(null);

  const [page, setPage] = useState(1);
  const [pageAlerts, setPageAlerts] = useState<Alert[]>([]);
  const [totalCount, setTotalCount] = useState(0);
  const [pageError, setPageError] = useState<string | null>(null);

  const [selectedAlert, setSelectedAlert] = useState<Alert | null>(null);
  const [selectedError, setSelectedError] = useState<string | null>(null);

  const totalPages = Math.max(1, Math.ceil(totalCount / PAGE_SIZE));

  const loadPage = useCallback(async () => {
    try {
      const result = await fetchAlertPage({
        offset: (page - 1) * PAGE_SIZE,
        limit: PAGE_SIZE,
        detectorName: detectorFilter === "ALL" ? undefined : detectorFilter,
        status: statusFilter === "ALL" ? undefined : statusFilter,
      });
      setPageAlerts(result.alerts);
      setTotalCount(result.totalCount);
      setPageError(null);
    } catch (e) {
      setPageError(e instanceof Error ? e.message : String(e));
    }
  }, [page, detectorFilter, statusFilter]);

  useEffect(() => {
    loadPage();
  }, [loadPage]);

  // Detail panel: fetches its own data by id, entirely independent of
  // loadPage()/pageAlerts above -- see this file's header comment.
  useEffect(() => {
    if (selectedAlertId === null) {
      setSelectedAlert(null);
      setSelectedError(null);
      return;
    }
    let cancelled = false;
    fetchAlert(selectedAlertId)
      .then((a) => {
        if (!cancelled) {
          setSelectedAlert(a);
          setSelectedError(null);
        }
      })
      .catch((e) => {
        if (!cancelled) setSelectedError(e instanceof Error ? e.message : String(e));
      });
    return () => {
      cancelled = true;
    };
  }, [selectedAlertId]);

  function changeDetectorFilter(value: string) {
    setDetectorFilter(value);
    setPage(1);
    setSelectedAlertId(null);
  }
  function changeStatusFilter(value: AlertStatus | "ALL") {
    setStatusFilter(value);
    setPage(1);
    setSelectedAlertId(null);
  }

  // A status change can affect this page's own contents (e.g. an active
  // status filter no longer matching) and the shared cross-tab poll --
  // refresh both, plus the detail panel's own independent fetch, which
  // just re-runs the effect above by re-selecting the same id.
  function handleStatusChanged() {
    onStatusChanged();
    loadPage();
    if (selectedAlertId !== null) {
      fetchAlert(selectedAlertId)
        .then(setSelectedAlert)
        .catch((e) => setSelectedError(e instanceof Error ? e.message : String(e)));
    }
  }

  const searched = useMemo(() => {
    const needle = search.trim().toLowerCase();
    if (!needle) return pageAlerts;
    return pageAlerts.filter((a) => {
      const haystack = `${a.instrument_id} ${a.account_ids.join(" ")}`.toLowerCase();
      return haystack.includes(needle);
    });
  }, [pageAlerts, search]);

  const sorted = useMemo(() => {
    const copy = [...searched];
    copy.sort((a, b) => {
      // "time" sorts by alert_id, not window_start_ns: the synthetic event
      // clock cpp/api/main.cpp's demo feed loop assigns isn't reliably
      // monotonic (it regresses at every session boundary -- see
      // cpp/pipeline/README.md), so alert_id (assigned in real insertion
      // order by the DB) is the field that actually tracks recency. Same
      // fix as list_recent_alerts() (cpp/db/alert_store.cpp) -- the TIME
      // column still *displays* window_start_ns (formatTime in AlertRow),
      // only the sort comparator changed.
      const av = sortKey === "time" ? a.alert_id : a.score;
      const bv = sortKey === "time" ? b.alert_id : b.score;
      return sortDir === "asc" ? av - bv : bv - av;
    });
    return copy;
  }, [searched, sortKey, sortDir]);

  function handleSort(key: AlertSortKey) {
    if (key === sortKey) {
      setSortDir((d) => (d === "asc" ? "desc" : "asc"));
    } else {
      setSortKey(key);
      setSortDir("desc");
    }
  }

  return (
    <div className="alerts-grid">
      <section className="panel alerts-list-panel">
        <div className="panel-header">
          <h2>Alert Queue</h2>
          <div className="filters">
            <select value={detectorFilter} onChange={(e) => changeDetectorFilter(e.target.value)}>
              <option value="ALL">all detectors</option>
              {DETECTOR_NAMES.map((name) => (
                <option key={name} value={name}>
                  {name}
                </option>
              ))}
            </select>
            <select value={statusFilter} onChange={(e) => changeStatusFilter(e.target.value as AlertStatus | "ALL")}>
              {STATUS_OPTIONS.map((s) => (
                <option key={s} value={s}>
                  {s === "ALL" ? "all statuses" : s}
                </option>
              ))}
            </select>
            <input
              placeholder="search: instrument or account (this page)"
              value={search}
              onChange={(e) => setSearch(e.target.value)}
            />
          </div>
        </div>

        <div className="alerts-list-scroll">
          {pageError ? (
            <p className="empty-state">Couldn't load alerts: {pageError}</p>
          ) : sorted.length === 0 ? (
            <p className="empty-state">No alerts match the current filters.</p>
          ) : (
            <>
              <AlertRowHeader sortKey={sortKey} sortDir={sortDir} onSort={handleSort} />
              {sorted.map((alert) => (
                <AlertRow
                  key={alert.alert_id}
                  alert={alert}
                  onStatusChanged={handleStatusChanged}
                  onSelectInstrument={onSelectInstrument}
                  selected={alert.alert_id === selectedAlertId}
                  onSelect={(a) => setSelectedAlertId(a.alert_id)}
                />
              ))}
            </>
          )}
        </div>

        <div className="alerts-pagination">
          <div className="alerts-pagination-nav">
            <button type="button" disabled={page <= 1} onClick={() => setPage((p) => p - 1)}>
              [&lt; prev]
            </button>
            <span className="tabular">
              page {page} of {totalPages}
            </span>
            <button type="button" disabled={page >= totalPages} onClick={() => setPage((p) => p + 1)}>
              [next &gt;]
            </button>
          </div>
          <span className="alerts-pagination-total tabular">{totalCount.toLocaleString()} total alerts</span>
        </div>
      </section>

      <section className="panel alerts-detail-panel">
        <div className="panel-header">
          <h2>Evidence</h2>
        </div>
        {selectedError ? (
          <p className="empty-state">Couldn't load this alert: {selectedError}</p>
        ) : (
          <AlertDetail alert={selectedAlert} onStatusChanged={handleStatusChanged} onSelectInstrument={onSelectInstrument} />
        )}
      </section>
    </div>
  );
}
