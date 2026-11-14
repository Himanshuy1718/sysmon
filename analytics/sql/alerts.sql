-- Alerts produced by the anomaly detector. Applied automatically on startup.
-- Requires the server's tables (nodes, metrics), created by sysmon_server.

CREATE TABLE IF NOT EXISTS alerts (
    id            BIGSERIAL PRIMARY KEY,
    host          TEXT        NOT NULL REFERENCES nodes(host),
    kind          TEXT        NOT NULL,   -- cpu_high, mem_high, memory_leak, spike_<metric>, node_down
    severity      TEXT        NOT NULL CHECK (severity IN ('info', 'warning', 'critical')),
    message       TEXT        NOT NULL,   -- human-readable, also used by the AI assistant later
    details       JSONB       NOT NULL DEFAULT '{}',  -- numbers behind the message
    started_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen_at  TIMESTAMPTZ NOT NULL DEFAULT now(),  -- updated while the problem continues
    resolved_at   TIMESTAMPTZ                          -- NULL = still open
);

-- DEDUPLICATION, enforced by the database itself: at most ONE open alert per
-- (host, kind). A problem that lasts 10 minutes is one alert updated 60
-- times, not 60 alerts. A "partial" index only covers rows matching WHERE.
CREATE UNIQUE INDEX IF NOT EXISTS alerts_one_open_per_kind
    ON alerts (host, kind) WHERE resolved_at IS NULL;

CREATE INDEX IF NOT EXISTS alerts_started_idx ON alerts (started_at DESC);
