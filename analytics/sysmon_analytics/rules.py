"""Anomaly detection rules.

Every rule is a PURE FUNCTION: a list of samples in, a Finding (or None) out.
No database, no clock, no global state, so each rule can be unit-tested with
hand-made data where the right answer is known.
"""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass, field
from typing import Any, Sequence

import numpy as np

from .config import Config, LeakRule, SpikeRule, ThresholdRule


@dataclass(frozen=True)
class Sample:
    ts: float                 # Unix seconds
    cpu_pct: float
    mem_pct: float
    mem_used_kb: float = 0
    mem_total_kb: float = 0
    disk_read_kbps: float = 0
    disk_write_kbps: float = 0
    net_rx_kbps: float = 0
    net_tx_kbps: float = 0
    top_cpu: tuple = ()       # ({"pid":..,"name":..,"cpu":..,"rss_kb":..}, ...)
    top_mem: tuple = ()


@dataclass(frozen=True)
class Finding:
    kind: str                 # e.g. "cpu_high"; at most one open alert per (host, kind)
    severity: str             # info | warning | critical
    message: str              # human-readable explanation
    details: dict[str, Any] = field(default_factory=dict)


LABELS = {
    "cpu_pct": "CPU",
    "mem_pct": "memory",
    "disk_read_kbps": "disk read",
    "disk_write_kbps": "disk write",
    "net_rx_kbps": "network receive",
    "net_tx_kbps": "network send",
}


def _window(samples: Sequence[Sample], seconds: float) -> list[Sample]:
    """Samples from the last `seconds`, measured from the newest sample."""
    if not samples:
        return []
    end = samples[-1].ts
    return [s for s in samples if s.ts >= end - seconds]


def top_process(samples: Sequence[Sample], key: str = "top_cpu") -> dict | None:
    """The process that was most often at #1 in the window, with its average usage.
    This turns "CPU is high" into "CPU is high because of X" — the most useful
    part of an alert."""
    firsts = [getattr(s, key)[0] for s in samples if getattr(s, key)]
    if not firsts:
        return None
    name, count = Counter(p["name"] for p in firsts).most_common(1)[0]
    mine = [p for p in firsts if p["name"] == name]
    return {
        "name": name,
        "pid": mine[-1]["pid"],
        "avg_cpu": round(float(np.mean([p["cpu"] for p in mine])), 1),
        "rss_mb": round(mine[-1]["rss_kb"] / 1024, 1),
        "share_of_window": round(count / len(samples), 2),
    }


# --------------------------------------------------------------------------- Rule 1
def sustained_threshold(samples: Sequence[Sample], metric: str, rule: ThresholdRule,
                        kind: str) -> Finding | None:
    """Fires if EVERY sample in the last `duration_s` is above `threshold`.

    "Every" (not "average") means one short blip never fires the alert.
    We also require the window to really span ~duration_s, so 2 samples right
    after start-up can't fire it."""
    win = _window(samples, rule.duration_s)
    if len(win) < 3 or win[-1].ts - win[0].ts < 0.8 * rule.duration_s:
        return None  # not enough history yet
    values = [getattr(s, metric) for s in win]
    if min(values) <= rule.threshold:
        return None

    label = LABELS.get(metric, metric)
    span = win[-1].ts - win[0].ts
    details = {
        "metric": metric,
        "avg": round(float(np.mean(values)), 1),
        "max": round(float(max(values)), 1),
        "threshold": rule.threshold,
        "duration_s": round(span),
    }
    msg = f"{label} above {rule.threshold:g}% for {span:.0f}s (avg {details['avg']}%, max {details['max']}%)"
    culprit = top_process(win, "top_cpu" if metric == "cpu_pct" else "top_mem")
    if culprit:
        details["top_process"] = culprit
        if metric == "cpu_pct":
            msg += f"; top process: {culprit['name']} (pid {culprit['pid']}, {culprit['avg_cpu']}% CPU)"
        else:
            msg += f"; largest process: {culprit['name']} (pid {culprit['pid']}, {culprit['rss_mb']} MB)"
    return Finding(kind, rule.severity, msg, details)


def recovered(samples: Sequence[Sample], metric: str, rule: ThresholdRule) -> bool:
    """HYSTERESIS: an open threshold alert closes only when the recent average
    falls below `resolve_below` (lower than the firing threshold). Without the
    gap, a value hovering around 90% would open and close an alert every pass."""
    recent = _window(samples, min(15.0, rule.duration_s))
    if not recent:
        return False
    return float(np.mean([getattr(s, metric) for s in recent])) < rule.resolve_below


# --------------------------------------------------------------------------- Rule 2
def zscore_spike(samples: Sequence[Sample], metric: str, rule: SpikeRule) -> Finding | None:
    """Sudden jump compared with this machine's own normal behaviour.

        baseline = samples from [now - baseline_s, now - recent_s)
        recent   = samples from the last recent_s seconds
        z = (mean(recent) - mean(baseline)) / std(baseline)

    z measures "how many standard deviations above normal". z >= 4 is very
    unlikely by chance. Because the baseline is per machine, a database server
    that always writes 50 MB/s is normal for itself and doesn't alert."""
    if not samples:
        return None
    end = samples[-1].ts
    baseline = [getattr(s, metric) for s in samples
                if end - rule.baseline_s <= s.ts < end - rule.recent_s]
    recent = [getattr(s, metric) for s in samples if s.ts >= end - rule.recent_s]
    if len(baseline) < rule.min_baseline_samples or not recent:
        return None

    mean, std = float(np.mean(baseline)), float(np.std(baseline))
    value = float(np.mean(recent))
    delta = value - mean
    min_delta = rule.min_delta.get(metric, 0.0)
    # Guard against std == 0 on a perfectly flat baseline (division by zero).
    z = delta / std if std > 1e-9 else (float("inf") if delta > 0 else 0.0)
    if z < rule.z_threshold or delta < min_delta:
        return None

    label = LABELS.get(metric, metric)
    unit = "%" if metric.endswith("_pct") else " kB/s"
    z_text = "∞" if z == float("inf") else f"{z:.1f}"
    msg = (f"Sudden {label} spike: {value:.1f}{unit} over the last {rule.recent_s:g}s "
           f"vs normal {mean:.1f}{unit} (z = {z_text})")
    details = {"metric": metric, "recent_avg": round(value, 2), "baseline_avg": round(mean, 2),
               "baseline_std": round(std, 2), "z": None if z == float("inf") else round(z, 2)}
    if metric == "cpu_pct":
        culprit = top_process(_window(samples, rule.recent_s))
        if culprit:
            details["top_process"] = culprit
            msg += f"; top process: {culprit['name']}"
    return Finding(f"spike_{metric}", rule.severity, msg, details)


# --------------------------------------------------------------------------- Rule 3
def memory_leak(samples: Sequence[Sample], rule: LeakRule) -> Finding | None:
    """Memory growing steadily = possible leak.

    Fits a straight line y = slope*x + b (least squares) to memory % over time.
      - slope: growth in % per minute
      - R^2 (0..1): how well the line fits. 1.0 = perfectly steady growth.
    Requiring a high R^2 separates a leak (steady climb) from normal usage
    that goes up and down. From the slope we can estimate the time until
    memory is full: (100 - current%) / slope."""
    win = _window(samples, rule.window_s)
    if len(win) < 5 or win[-1].ts - win[0].ts < rule.min_span_s:
        return None

    x = np.array([(s.ts - win[0].ts) / 60.0 for s in win])  # minutes
    y = np.array([s.mem_pct for s in win])
    slope, intercept = np.polyfit(x, y, 1)
    predicted = slope * x + intercept
    ss_res = float(np.sum((y - predicted) ** 2))
    ss_tot = float(np.sum((y - y.mean()) ** 2))
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 1e-12 else 0.0

    if slope < rule.min_slope_pct_per_min or r2 < rule.min_r2:
        return None

    minutes_to_full = (100.0 - y[-1]) / slope
    details = {
        "slope_pct_per_min": round(float(slope), 2),
        "r2": round(r2, 3),
        "mem_pct_now": round(float(y[-1]), 1),
        "minutes_to_full": round(float(minutes_to_full), 1),
        "window_s": round(win[-1].ts - win[0].ts),
    }
    msg = (f"Memory rising steadily: +{slope:.2f}% per minute for {details['window_s']}s "
           f"(R² = {r2:.2f}), now {y[-1]:.1f}%; full in about {minutes_to_full:.0f} min if this continues")

    # Which process grew? For each PID in the newest sample, compare its memory now
    # with the FIRST sample in the window where it appears. (A leaking program
    # often starts after the window begins, or is too small at first to be in
    # the top-5 list; comparing only first vs last sample would miss it.)
    first_seen: dict[int, dict] = {}
    for s in win:
        for p in s.top_mem:
            first_seen.setdefault(p["pid"], p)
    last = {p["pid"]: p for p in win[-1].top_mem}
    growth = [(last[pid]["rss_kb"] - first_seen[pid]["rss_kb"], last[pid]) for pid in last]
    if growth:
        grew_kb, proc = max(growth, key=lambda g: g[0])
        if grew_kb > 0:
            details["growing_process"] = {"name": proc["name"], "pid": proc["pid"],
                                          "grew_mb": round(grew_kb / 1024, 1)}
            msg += f"; fastest-growing process: {proc['name']} (pid {proc['pid']}, +{grew_kb / 1024:.0f} MB)"
    return Finding("memory_leak", rule.severity, msg, details)


# --------------------------------------------------------------------------- all rules
@dataclass
class HostResult:
    findings: dict[str, Finding]  # kind -> finding: alerts that should be open now
    keep_open: set[str]           # kinds not firing, but not recovered enough to close


def evaluate_host(samples: Sequence[Sample], cfg: Config, open_kinds: set[str]) -> HostResult:
    """Runs every rule for one host. `open_kinds` = alert kinds currently open
    for this host (needed for hysteresis)."""
    findings: dict[str, Finding] = {}
    keep: set[str] = set()

    for metric, rule, kind in (("cpu_pct", cfg.cpu_high, "cpu_high"),
                               ("mem_pct", cfg.mem_high, "mem_high")):
        f = sustained_threshold(samples, metric, rule, kind)
        if f:
            findings[kind] = f
        elif kind in open_kinds and not recovered(samples, metric, rule):
            keep.add(kind)  # between resolve_below and threshold: stay open

    for metric in cfg.spikes.min_delta:
        f = zscore_spike(samples, metric, cfg.spikes)
        if f:
            findings[f.kind] = f

    f = memory_leak(samples, cfg.memory_leak)
    if f:
        findings[f.kind] = f

    return HostResult(findings, keep)
