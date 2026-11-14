"""Database access for the detector (psycopg 3)."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import psycopg
from psycopg.types.json import Jsonb

from .rules import Finding, Sample

ALERTS_SQL = Path(__file__).resolve().parent.parent / "sql" / "alerts.sql"


class Store:
    def __init__(self, conninfo: str, **connect_kwargs: Any):
        self.conninfo = conninfo
        self.connect_kwargs = connect_kwargs
        self.conn: psycopg.Connection | None = None

    # ------------------------------------------------------------------ connection
    def connect(self) -> None:
        if self.conn is not None and not self.conn.closed:
            return
        # autocommit: each statement commits on its own; we use explicit
        # transactions only where several statements must succeed together.
        self.conn = psycopg.connect(self.conninfo, autocommit=True, **self.connect_kwargs)

    def close(self) -> None:
        if self.conn is not None:
            self.conn.close()
            self.conn = None

    def ensure_schema(self) -> None:
        self.connect()
        with self.conn.cursor() as cur:
            cur.execute("SELECT to_regclass('nodes') IS NOT NULL AND to_regclass('metrics') IS NOT NULL")
            if not cur.fetchone()[0]:
                raise RuntimeError("tables 'nodes'/'metrics' not found - start sysmon_server with --db first")
            cur.execute("SET client_min_messages = warning")
            cur.execute(ALERTS_SQL.read_text())

    # ------------------------------------------------------------------ reads
    def fetch_samples(self, history_s: float) -> dict[str, list[Sample]]:
        """Recent samples for every host, oldest first. Uses the (ts) index."""
        self.connect()
        sql = """
            SELECT host, extract(epoch FROM ts)::float8, cpu_pct, mem_pct, mem_used_kb, mem_total_kb,
                   disk_read_kbps, disk_write_kbps, net_rx_kbps, net_tx_kbps, top_cpu, top_mem
            FROM metrics
            WHERE ts > now() - make_interval(secs => %s)
            ORDER BY host, ts
        """
        out: dict[str, list[Sample]] = {}
        with self.conn.cursor() as cur:
            cur.execute(sql, (history_s,))
            for row in cur:
                host = row[0]
                out.setdefault(host, []).append(Sample(
                    ts=row[1], cpu_pct=row[2], mem_pct=row[3], mem_used_kb=row[4], mem_total_kb=row[5],
                    disk_read_kbps=row[6], disk_write_kbps=row[7], net_rx_kbps=row[8], net_tx_kbps=row[9],
                    top_cpu=tuple(row[10] or ()), top_mem=tuple(row[11] or ()),
                ))
        return out

    def db_now(self) -> float:
        self.connect()
        with self.conn.cursor() as cur:
            cur.execute("SELECT extract(epoch FROM now())::float8")
            return cur.fetchone()[0]

    def down_nodes(self) -> dict[str, float]:
        """host -> last_seen (Unix seconds) for nodes the server marked DOWN."""
        self.connect()
        with self.conn.cursor() as cur:
            cur.execute("SELECT host, extract(epoch FROM last_seen)::float8 FROM nodes WHERE status = 'DOWN'")
            return {h: ts for h, ts in cur.fetchall()}

    def open_alerts(self) -> dict[tuple[str, str], int]:
        """(host, kind) -> alert id, for alerts not yet resolved."""
        self.connect()
        with self.conn.cursor() as cur:
            cur.execute("SELECT host, kind, id FROM alerts WHERE resolved_at IS NULL")
            return {(h, k): i for h, k, i in cur.fetchall()}

    # ------------------------------------------------------------------ writes
    def upsert_alert(self, host: str, f: Finding) -> tuple[int, bool]:
        """Opens a new alert, or refreshes the open one for (host, kind).
        Returns (id, created). ON CONFLICT targets the partial unique index,
        so the database guarantees no duplicates even with two detectors running."""
        self.connect()
        sql = """
            INSERT INTO alerts (host, kind, severity, message, details)
            VALUES (%s, %s, %s, %s, %s)
            ON CONFLICT (host, kind) WHERE resolved_at IS NULL
            DO UPDATE SET last_seen_at = now(), message = EXCLUDED.message,
                          details = EXCLUDED.details, severity = EXCLUDED.severity
            RETURNING id, (xmax = 0) AS created   -- xmax = 0 only for a freshly inserted row
        """
        with self.conn.cursor() as cur:
            cur.execute(sql, (host, f.kind, f.severity, f.message, Jsonb(f.details)))
            alert_id, created = cur.fetchone()
            return alert_id, created

    def touch_alert(self, alert_id: int) -> None:
        self.connect()
        with self.conn.cursor() as cur:
            cur.execute("UPDATE alerts SET last_seen_at = now() WHERE id = %s", (alert_id,))

    def resolve_alert(self, alert_id: int) -> str | None:
        """Closes an alert. Returns its message (for logging)."""
        self.connect()
        with self.conn.cursor() as cur:
            cur.execute("UPDATE alerts SET resolved_at = now() WHERE id = %s AND resolved_at IS NULL "
                        "RETURNING message", (alert_id,))
            row = cur.fetchone()
            return row[0] if row else None
