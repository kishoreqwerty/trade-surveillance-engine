# dashboard/ — the React UI: design decisions

Companion to `P2_trade_surveillance_engine_architecture.md`'s module
table (`dashboard/ | React UI, reads from api/ only, no direct backend
access | api/`) and `cpp/api/README.md`. Own npm project (Vite + React +
TypeScript), independent of `cpp/` and `ml_service/`'s build tooling —
the only coupling is the documented REST contract in `cpp/api/README.md`.

## No mocked or hardcoded data, anywhere

Every number on screen comes from a real `fetch()` in `src/api.ts` against
a running `cpp/api_server` — there is no fixture file, no seeded array of
fake alerts, no placeholder instrument list. The instrument selector
defaults to the most recently fetched alert's own `instrument_id` (a
real, derived value) rather than a hardcoded symbol; the user can type any
instrument into the same field.

## The six build-guide deliverables, and what backs each one

| Deliverable | Component | Backed by |
|---|---|---|
| Live ticker | `LiveTicker.tsx` | polls `GET /api/orderbook/:id/snapshot` every 1.5s, shows best bid/ask + the real, ticking `sequence` number |
| Alert queue | `AlertQueue.tsx` | polls `GET /api/alerts` every 3s |
| Severity-coded alert cards | `AlertCard.tsx` | `score` mapped onto the dataviz skill's reserved status palette (`severity.ts`) — icon + color + text label together, never color alone |
| Order book depth visualization | `OrderBookDepth.tsx` | two-sided price ladder from the same snapshot poll, bar length proportional to `total_qty`, blue/red diverging pair for bid/ask |
| Event timeline | `EventTimeline.tsx` | every currently-loaded alert plotted on one shared time axis by `window_start_ns`, one row per detector (small multiples, not a second axis), colored by the fixed categorical palette |
| Compliance action buttons | `AlertCard.tsx` | `PATCH /api/alerts/:id/status`, state-machine-gated (only valid next transitions render as buttons) |

## Palette

Uses the dataviz skill's validated reference palette (`references/palette.md`)
unmodified — status colors (good/warning/serious/critical) for severity,
never reused for anything else; the fixed 8-slot categorical order for
detector identity, assigned once per detector name and never reassigned
(`severity.ts`'s `colorForDetector`); the blue↔red diverging pair for
bid/ask. Both light and dark mode are wired via the palette's own
`prefers-color-scheme` + `[data-theme]` pattern.

## A real bug this app's existence caught: CORS

The dashboard doesn't just consume `cpp/api/` — building it and actually
driving it in a real browser is what caught a real bug in the API server
(cross-origin requests were silently blocked; every `cpp-httplib`-based
test in `cpp/tests/api/` had passed because `curl`-like clients don't
enforce CORS). See `cpp/api/README.md`'s "A real bug only a browser
catches" section for the full story — it's documented on the API side
since that's where the fix lives, but it was this app's own real-browser
verification that found it.

## Running against the live pipeline

```bash
docker compose up -d timescaledb        # from the repo root
./build-bench/cpp/api/tse_api_server 8081   # the live demo server -- see cpp/api/README.md
cd dashboard
echo "VITE_API_BASE_URL=http://127.0.0.1:8081" > .env.local
npm install
npm run dev
```

`VITE_API_BASE_URL` defaults to `http://127.0.0.1:8081` (the demo
server's own default port) if unset.

## Verification

`npm run build` (typecheck via `tsc -b`, then the Vite production bundle)
and `npm run lint` (oxlint) both pass clean. Beyond that — actual visual
and interactive proof, not just "the code compiles" — a headless Chromium
session (driven via Playwright, installed ad hoc for this verification,
not a project dependency) navigated the running dev server against the
running live demo server and confirmed, with screenshots: 36 real alert
cards rendered from real TimescaleDB rows, the order-book depth ladder and
live ticker showing real bid/ask levels with a sequence number that
visibly increased across three separate polls, and a real compliance
action (clicking "Mark Under Review" on a live alert card) that produced
a visible status-pill change, confirmed against the database independently
via `AlertStore::get_alert()`. Zero browser console errors in the final run.
