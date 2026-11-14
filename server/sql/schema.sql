-- SysMon database schema. Applied automatically when the server starts.
-- Every statement is idempotent (IF NOT EXISTS), so re-running is harmless.

-- One row per monitored machine: its latest known state.
CREATE TABLE IF NOT EXISTS nodes (
    host        TEXT PRIMARY KEY,
    address     TEXT,
    status      TEXT        NOT NULL DEFAULT 'UP' CHECK (status IN ('UP', 'DOWN')),
    first_seen  TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen   TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- One row per snapshot: the time series. This table grows fastest.
CREATE TABLE IF NOT EXISTS metrics (
    id               BIGSERIAL PRIMARY KEY,
    host             TEXT             NOT NULL REFERENCES nodes(host),
    ts               TIMESTAMPTZ      NOT NULL,   -- when the agent took the sample
    cpu_pct          DOUBLE PRECISION NOT NULL,
    mem_pct          DOUBLE PRECISION NOT NULL,
    mem_used_kb      BIGINT           NOT NULL,
    mem_total_kb     BIGINT           NOT NULL,
    disk_read_kbps   DOUBLE PRECISION NOT NULL,
    disk_write_kbps  DOUBLE PRECISION NOT NULL,
    net_rx_kbps      DOUBLE PRECISION NOT NULL,
    net_tx_kbps      DOUBLE PRECISION NOT NULL,
    load1            DOUBLE PRECISION NOT NULL,
    load5            DOUBLE PRECISION NOT NULL,
    load15           DOUBLE PRECISION NOT NULL,
    -- Top processes vary in number and are always read as a whole with their
    -- snapshot, so JSONB is simpler than a separate table here.
    top_cpu          JSONB            NOT NULL DEFAULT '[]',
    top_mem          JSONB            NOT NULL DEFAULT '[]'
);

-- Almost every query is "metrics for host X in a time window, newest first".
-- A composite index on (host, ts) answers that without scanning the table.
CREATE INDEX IF NOT EXISTS metrics_host_ts_idx ON metrics (host, ts DESC);
-- For "what happened across all nodes in the last N minutes".
CREATE INDEX IF NOT EXISTS metrics_ts_idx ON metrics (ts DESC);

-- UP/DOWN history: when each node appeared and disappeared.
CREATE TABLE IF NOT EXISTS node_events (
    id     BIGSERIAL PRIMARY KEY,
    host   TEXT        NOT NULL REFERENCES nodes(host),
    ts     TIMESTAMPTZ NOT NULL,
    event  TEXT        NOT NULL CHECK (event IN ('UP', 'DOWN'))
);
CREATE INDEX IF NOT EXISTS node_events_host_ts_idx ON node_events (host, ts DESC);
