"""End-to-end detector tests against a REAL PostgreSQL.

Run with:  SYSMON_TEST_DB=postgresql://sysmon:sysmon@localhost:5433/sysmon pytest
Skipped when SYSMON_TEST_DB is not set. Each test runs in its own throw-away
PostgreSQL schema, so real data is never touched.
"""

import json
import os
import random
from pathlib import Path

import psycopg
import pytest

from sysmon_analytics.config import Config
from sysmon_analytics.detector import run_once
from sysmon_analytics.store import Store

DB = os.environ.get("SYSMON_TEST_DB")
SERVER_SCHEMA = Path(__file__).resolve().parents[2] / "server" / "sql" / "schema.sql"

pytestmark = pytest.mark.skipif(not DB, reason="set SYSMON_TEST_DB to run database tests")


@pytest.fixture
def db():
    schema = f"sysmon_pytest_{random.randrange(10**9)}"
    admin = psycopg.connect(DB, autocommit=True)
    admin.execute(f"CREATE SCHEMA {schema}")
    opts = f"-c search_path={schema}"
    conn = psycopg.connect(DB, autocommit=True, options=opts)
    conn.execute("SET client_min_messages = warning")
    conn.execute(SERVER_SCHEMA.read_text())  # the C++ server's tables
    store = Store(DB, options=opts)
    store.ensure_schema()                    # + the alerts table
    yield conn, store
    store.close()
    conn.close()
    admin.execute(f"DROP SCHEMA {schema} CASCADE")
    admin.close()


def add_metrics(conn, host, cpu_values, mem_pct=40.0, seconds_ago_start=None, top="worker"):
    """Inserts one sample per second ending now."""
    n = len(cpu_values)
    start = seconds_ago_start if seconds_ago_start is not None else n
    conn.execute("INSERT INTO nodes (host) VALUES (%s) ON CONFLICT DO NOTHING", (host,))
    with conn.cursor() as cur:
        for i, cpu in enumerate(cpu_values):
            cur.execute(
                """INSERT INTO metrics (host, ts, cpu_pct, mem_pct, mem_used_kb, mem_total_kb,
                       disk_read_kbps, disk_write_kbps, net_rx_kbps, net_tx_kbps, load1, load5, load15,
                       top_cpu, top_mem)
                   VALUES (%s, now() - make_interval(secs => %s), %s, %s, 1, 2, 0, 0, 0, 0, 0, 0, 0, %s, '[]')""",
                (host, start - i, cpu, mem_pct,
                 json.dumps([{"pid": 1234, "name": top, "cpu": cpu, "rss_kb": 1000}])))


def alerts(conn):
    return conn.execute(
        "SELECT host, kind, severity, message, resolved_at IS NULL AS open FROM alerts ORDER BY id").fetchall()


def test_opens_alert_with_culprit(db):
    conn, store = db
    add_metrics(conn, "web-1", [5] * 20 + [97] * 40, top="yes")
    add_metrics(conn, "web-2", [5] * 60)  # healthy neighbour

    r = run_once(store, Config())
    assert [(h, f.kind) for h, f in r.opened] == [("web-1", "cpu_high")]
    rows = alerts(conn)
    assert len(rows) == 1
    host, kind, severity, message, is_open = rows[0]
    assert (host, kind, severity, is_open) == ("web-1", "cpu_high", "warning", True)
    assert "yes" in message


def test_repeated_passes_do_not_duplicate(db):
    conn, store = db
    add_metrics(conn, "web-1", [97] * 60)
    run_once(store, Config())
    r2 = run_once(store, Config())
    r3 = run_once(store, Config())
    assert r2.opened == [] and r3.opened == []
    assert r3.updated == 1
    assert len(alerts(conn)) == 1  # still one alert, refreshed


def test_database_rejects_second_open_alert_for_same_kind(db):
    conn, _ = db
    conn.execute("INSERT INTO nodes (host) VALUES ('x')")
    conn.execute("INSERT INTO alerts (host, kind, severity, message) VALUES ('x','cpu_high','warning','a')")
    with pytest.raises(psycopg.errors.UniqueViolation):
        conn.execute("INSERT INTO alerts (host, kind, severity, message) VALUES ('x','cpu_high','warning','b')")


def test_resolves_after_recovery_with_hysteresis(db):
    conn, store = db
    add_metrics(conn, "web-1", [97] * 60, seconds_ago_start=120)  # high from 120 s ago to 60 s ago
    # ...then 85% (between resolve_below=80 and threshold=90) for the last 30 s
    add_metrics(conn, "web-1", [85] * 30)
    conn.execute("INSERT INTO alerts (host, kind, severity, message) VALUES ('web-1','cpu_high','warning','old')")

    r = run_once(store, Config())
    assert r.resolved == []            # 85% is not "recovered": stays open
    assert alerts(conn)[0][4] is True

    add_metrics(conn, "web-1", [10] * 20, seconds_ago_start=0)  # newest samples: back to normal
    conn.execute("DELETE FROM metrics WHERE cpu_pct = 85")
    r = run_once(store, Config())
    assert [(h, k) for h, k, _ in r.resolved] == [("web-1", "cpu_high")]
    assert alerts(conn)[0][4] is False


def test_node_down_alert_opens_and_resolves(db):
    conn, store = db
    add_metrics(conn, "db-1", [5] * 30)
    conn.execute("UPDATE nodes SET status = 'DOWN', last_seen = now() - interval '40 seconds' WHERE host='db-1'")
    r = run_once(store, Config())
    assert [(h, f.kind, f.severity) for h, f in r.opened] == [("db-1", "node_down", "critical")]

    conn.execute("UPDATE nodes SET status = 'UP' WHERE host='db-1'")
    r = run_once(store, Config())
    assert [(h, k) for h, k, _ in r.resolved] == [("db-1", "node_down")]


def test_stale_host_alerts_are_left_alone(db):
    conn, store = db
    add_metrics(conn, "old-1", [97] * 40, seconds_ago_start=300)  # data stopped 4+ min ago
    conn.execute("INSERT INTO alerts (host, kind, severity, message) VALUES ('old-1','cpu_high','warning','x')")
    r = run_once(store, Config())
    assert r.resolved == []  # can't know it recovered: no fresh data
