"""Anomaly detector service.

Every `interval_s` seconds:
  1. load the last `history_s` seconds of metrics for every host
  2. run every rule per host                         (rules.py, pure functions)
  3. open / refresh alerts that fire, resolve alerts that stopped firing

Usage:
  python -m sysmon_analytics.detector --db postgresql://sysmon:sysmon@localhost:5433/sysmon
  python -m sysmon_analytics.detector --db ... --once          # one pass, then exit
"""

from __future__ import annotations

import argparse
import os
import signal
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from .config import Config, load_config
from .rules import Finding, evaluate_host
from .store import Store

DEFAULT_CONFIG = Path(__file__).resolve().parent.parent / "config.toml"


@dataclass
class PassResult:
    opened: list[tuple[str, Finding]] = field(default_factory=list)
    updated: int = 0
    resolved: list[tuple[str, str, str]] = field(default_factory=list)  # (host, kind, message)
    hosts: int = 0


def run_once(store: Store, cfg: Config) -> PassResult:
    result = PassResult()
    samples_by_host = store.fetch_samples(cfg.history_s)
    now = store.db_now()
    open_alerts = store.open_alerts()

    desired: dict[tuple[str, str], Finding] = {}
    keep: set[tuple[str, str]] = set()

    # ---- metric rules, per host
    for host, samples in samples_by_host.items():
        result.hosts += 1
        open_kinds = {k for (h, k) in open_alerts if h == host}
        if now - samples[-1].ts > cfg.stale_after_s:
            # No fresh data: we can't tell if things recovered, so leave its
            # alerts as they are. node_down (below) reports the silence itself.
            keep.update((host, k) for k in open_kinds)
            continue
        hr = evaluate_host(samples, cfg, open_kinds)
        for kind, f in hr.findings.items():
            desired[(host, kind)] = f
        keep.update((host, k) for k in hr.keep_open)

    # ---- node down (the server's watchdog sets nodes.status = 'DOWN')
    for host, last_seen in store.down_nodes().items():
        silent = now - last_seen
        desired[(host, "node_down")] = Finding(
            "node_down", "critical",
            f"Node is DOWN: no data for {silent:.0f}s (last seen {datetime.fromtimestamp(last_seen):%H:%M:%S})",
            {"last_seen": last_seen, "silent_s": round(silent)})

    # ---- apply: open/refresh what fires, resolve what stopped
    for (host, kind), f in desired.items():
        _, created = store.upsert_alert(host, f)
        if created:
            result.opened.append((host, f))
        else:
            result.updated += 1
    for (host, kind), alert_id in open_alerts.items():
        if (host, kind) in desired:
            continue
        if (host, kind) in keep:
            store.touch_alert(alert_id)
            continue
        msg = store.resolve_alert(alert_id)
        if msg is not None:
            result.resolved.append((host, kind, msg))
    return result


# --------------------------------------------------------------------------- CLI
COLORS = {"critical": "\033[31m", "warning": "\033[33m", "info": "\033[36m", "ok": "\033[32m"}
RESET = "\033[0m"


def _log(level: str, text: str) -> None:
    color = COLORS.get(level, "") if sys.stdout.isatty() else ""
    reset = RESET if color else ""
    print(f"{datetime.now():%H:%M:%S}  {color}{level.upper():8}{reset} {text}", flush=True)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="SysMon anomaly detector")
    p.add_argument("--db", default=os.environ.get("SYSMON_DB"),
                   help="PostgreSQL connection string (default: $SYSMON_DB)")
    p.add_argument("--config", default=str(DEFAULT_CONFIG), help="TOML config file")
    p.add_argument("--once", action="store_true", help="run a single pass and exit")
    args = p.parse_args(argv)
    if not args.db:
        p.error("--db is required (or set SYSMON_DB)")

    cfg = load_config(args.config)
    store = Store(args.db)

    stop = False

    def on_signal(*_):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    _log("info", f"detector started (every {cfg.interval_s:g}s, config {args.config})")
    schema_ready = False
    while not stop:
        try:
            if not schema_ready:
                store.ensure_schema()
                schema_ready = True
            r = run_once(store, cfg)
            for host, f in r.opened:
                _log(f.severity, f"[{host}] OPEN  {f.kind}: {f.message}")
            for host, kind, msg in r.resolved:
                _log("ok", f"[{host}] RESOLVED {kind}")
        except Exception as e:  # DB down, etc.: report and try again next pass
            _log("warning", f"pass failed: {e}")
            store.close()
        if args.once:
            break
        deadline = time.monotonic() + cfg.interval_s
        while not stop and time.monotonic() < deadline:
            time.sleep(0.2)

    store.close()
    _log("info", "detector stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
