-- Useful queries for exploring SysMon data.
-- Run all:   psql postgresql://sysmon:sysmon@localhost:5432/sysmon -f scripts/queries.sql
-- Or open a shell:  psql postgresql://sysmon:sysmon@localhost:5432/sysmon   and paste one.

-- 1. Every node and whether it is up right now.
SELECT host, status, address, last_seen
FROM nodes
ORDER BY host;

-- 2. UP/DOWN history, newest first.
SELECT host, event, ts
FROM node_events
ORDER BY ts DESC
LIMIT 20;

-- 3. Latest sample per node (DISTINCT ON keeps the first row per host).
SELECT DISTINCT ON (host)
       host, ts, round(cpu_pct::numeric, 1) AS cpu, round(mem_pct::numeric, 1) AS mem
FROM metrics
ORDER BY host, ts DESC;

-- 4. Average and peak CPU per node over the last 10 minutes.
SELECT host,
       count(*)                         AS samples,
       round(avg(cpu_pct)::numeric, 2)  AS avg_cpu,
       round(max(cpu_pct)::numeric, 2)  AS max_cpu
FROM metrics
WHERE ts > now() - interval '10 minutes'
GROUP BY host
ORDER BY avg_cpu DESC;

-- 5. CPU per minute for one node (a simple time series for a chart).
SELECT date_trunc('minute', ts) AS minute,
       round(avg(cpu_pct)::numeric, 2) AS avg_cpu
FROM metrics
WHERE host = 'node-1'
GROUP BY 1
ORDER BY 1;

-- 6. Moments when CPU was above 50%, with the process responsible.
--    (JSONB: top_cpu->0 is the first array element, ->>'name' reads a field as text)
SELECT host, ts, round(cpu_pct::numeric, 1) AS cpu,
       top_cpu->0->>'name' AS top_process,
       round((top_cpu->0->>'cpu')::numeric, 1) AS process_cpu
FROM metrics
WHERE cpu_pct > 50
ORDER BY ts DESC
LIMIT 20;

-- 7. Gaps in the data (a node silent for longer than 3 s between samples).
SELECT host, prev_ts, ts, round(gap_s::numeric, 1) AS gap_s
FROM (
    SELECT host, ts,
           lag(ts) OVER (PARTITION BY host ORDER BY ts) AS prev_ts,
           extract(epoch FROM ts - lag(ts) OVER (PARTITION BY host ORDER BY ts)) AS gap_s
    FROM metrics
) t
WHERE gap_s > 3
ORDER BY ts DESC;

-- 8. Table sizes (the metrics table grows fastest).
SELECT relname AS table, pg_size_pretty(pg_total_relation_size(relid)) AS size
FROM pg_catalog.pg_statio_user_tables
ORDER BY pg_total_relation_size(relid) DESC;

-- 9. Retention: delete samples older than 7 days (run periodically, e.g. cron).
-- DELETE FROM metrics WHERE ts < now() - interval '7 days';

-- 10. Alerts (Milestone 5): open ones first, then the most recent.
SELECT host, kind, severity,
       CASE WHEN resolved_at IS NULL THEN 'OPEN' ELSE 'resolved' END AS state,
       started_at::time(0) AS started,
       round(extract(epoch FROM coalesce(resolved_at, now()) - started_at)) AS lasted_s,
       left(message, 90) AS message
FROM alerts
ORDER BY resolved_at IS NOT NULL, started_at DESC
LIMIT 20;
