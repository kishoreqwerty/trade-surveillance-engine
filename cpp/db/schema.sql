-- TimescaleDB schema for the alert store (Phase 8). Every statement is
-- idempotent (IF NOT EXISTS) -- AlertStore::apply_schema() runs this on
-- every startup, real or test, not just once at provisioning time.
--
-- `event_time` on each table is derived from the row's own nanosecond
-- timestamp (timestamp_ns for orders/trades, window_end_ns for alerts) at
-- INSERT time, not wall-clock insertion time -- see alert_store.cpp. This
-- is what hypertable partitioning and time-range queries key on, while the
-- authoritative nanosecond value (CLAUDE.md: "all timestamps ... stored as
-- int64_t epoch nanos") is preserved unchanged alongside it.

CREATE EXTENSION IF NOT EXISTS timescaledb;

-- One row per Order event (New/Cancelled/Replaced), exactly as fix::Order
-- carries it -- see cpp/fix/types.hpp.
CREATE TABLE IF NOT EXISTS orders (
    order_id        TEXT NOT NULL,
    orig_order_id   TEXT NOT NULL,
    account_id      TEXT NOT NULL,
    instrument_id   TEXT NOT NULL,
    side            TEXT NOT NULL,
    price           DOUBLE PRECISION NOT NULL,
    qty             BIGINT NOT NULL,
    order_type      TEXT NOT NULL,
    status          TEXT NOT NULL,
    venue           TEXT NOT NULL,
    timestamp_ns    BIGINT NOT NULL,
    event_time      TIMESTAMPTZ NOT NULL,
    PRIMARY KEY (order_id, event_time)
);
SELECT create_hypertable('orders', 'event_time', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS idx_orders_account_time ON orders (account_id, event_time DESC);
CREATE INDEX IF NOT EXISTS idx_orders_instrument_time ON orders (instrument_id, event_time DESC);

-- One row per Execution -- see cpp/fix/types.hpp.
CREATE TABLE IF NOT EXISTS trades (
    trade_id                 TEXT NOT NULL,
    order_id                 TEXT NOT NULL,
    account_id                TEXT NOT NULL,
    counterparty_account_id     TEXT NOT NULL,
    instrument_id                TEXT NOT NULL,
    side                          TEXT NOT NULL,
    price                          DOUBLE PRECISION NOT NULL,
    qty                             BIGINT NOT NULL,
    venue                            TEXT NOT NULL,
    timestamp_ns                     BIGINT NOT NULL,
    event_time                       TIMESTAMPTZ NOT NULL,
    PRIMARY KEY (trade_id, event_time)
);
SELECT create_hypertable('trades', 'event_time', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS idx_trades_account_time ON trades (account_id, event_time DESC);
CREATE INDEX IF NOT EXISTS idx_trades_instrument_time ON trades (instrument_id, event_time DESC);

-- One row per Alert -- see cpp/detectors/alert.hpp. account_ids/order_ids
-- are stored as native Postgres arrays (a detector alert can implicate more
-- than one account/order -- e.g. WashTradeDetector's two related accounts,
-- FrontRunningDetector's leader+follower pair). model_version and
-- book_snapshot_sequence are nullable: only MlAnomalyDetector-sourced
-- alerts have a model_version, and see alert.hpp for why
-- book_snapshot_sequence is std::optional on the C++ side even though every
-- current detector populates it. `status` is case-management state (Phase
-- 9's concern per alert.hpp's own comment on why detectors::Alert doesn't
-- carry it) -- a detector never sets it; it starts 'OPEN' and only ever
-- changes via AlertStore::update_alert_status(), which api/'s compliance
-- action endpoints call.
CREATE TABLE IF NOT EXISTS alerts (
    alert_id                  BIGSERIAL,
    detector_name               TEXT NOT NULL,
    score                         DOUBLE PRECISION NOT NULL,
    instrument_id                  TEXT NOT NULL,
    account_ids                     TEXT[] NOT NULL,
    order_ids                        TEXT[] NOT NULL,
    window_start_ns                   BIGINT NOT NULL,
    window_end_ns                      BIGINT NOT NULL,
    evidence                            TEXT NOT NULL,
    model_version                        TEXT,
    book_snapshot_sequence                 BIGINT,
    status                                  TEXT NOT NULL DEFAULT 'OPEN'
        CHECK (status IN ('OPEN', 'UNDER_REVIEW', 'ESCALATED', 'CLOSED')),
    event_time                              TIMESTAMPTZ NOT NULL,
    PRIMARY KEY (alert_id, event_time)
);
-- Idempotent migration for a database that already had `alerts` from
-- before `status` existed (this project's own docker-compose instance,
-- mid-development, included) -- CREATE TABLE IF NOT EXISTS alone would
-- silently skip adding the column to an already-created table.
ALTER TABLE alerts ADD COLUMN IF NOT EXISTS status TEXT NOT NULL DEFAULT 'OPEN'
    CHECK (status IN ('OPEN', 'UNDER_REVIEW', 'ESCALATED', 'CLOSED'));
SELECT create_hypertable('alerts', 'event_time', if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS idx_alerts_detector_time ON alerts (detector_name, event_time DESC);
CREATE INDEX IF NOT EXISTS idx_alerts_instrument_time ON alerts (instrument_id, event_time DESC);
CREATE INDEX IF NOT EXISTS idx_alerts_account_ids ON alerts USING GIN (account_ids);
CREATE INDEX IF NOT EXISTS idx_alerts_status ON alerts (status);
