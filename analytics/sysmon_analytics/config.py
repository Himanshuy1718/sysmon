"""Detector configuration: dataclasses with defaults, optionally loaded from TOML."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path


@dataclass(frozen=True)
class ThresholdRule:
    threshold: float
    resolve_below: float
    duration_s: float
    severity: str


@dataclass(frozen=True)
class SpikeRule:
    z_threshold: float = 4.0
    baseline_s: float = 600
    recent_s: float = 20
    min_baseline_samples: int = 20
    severity: str = "info"
    min_delta: dict[str, float] = field(default_factory=lambda: {
        "cpu_pct": 25.0,
        "disk_write_kbps": 5000.0,
        "disk_read_kbps": 5000.0,
        "net_rx_kbps": 2000.0,
        "net_tx_kbps": 2000.0,
    })


@dataclass(frozen=True)
class LeakRule:
    window_s: float = 120
    min_span_s: float = 90
    min_slope_pct_per_min: float = 0.5
    min_r2: float = 0.9
    severity: str = "warning"


@dataclass(frozen=True)
class Config:
    interval_s: float = 10
    history_s: float = 900
    stale_after_s: float = 60
    cpu_high: ThresholdRule = ThresholdRule(90.0, 80.0, 30, "warning")
    mem_high: ThresholdRule = ThresholdRule(90.0, 85.0, 60, "critical")
    spikes: SpikeRule = SpikeRule()
    memory_leak: LeakRule = LeakRule()


def load_config(path: str | Path | None) -> Config:
    """Reads a TOML file; anything missing keeps its default."""
    if path is None:
        return Config()
    with open(path, "rb") as f:
        raw = tomllib.load(f)

    d = Config()
    det = raw.get("detector", {})
    spikes = dict(raw.get("spikes", {}))
    min_delta = spikes.pop("min_delta", None)

    def threshold(name: str, default: ThresholdRule) -> ThresholdRule:
        r = raw.get(name, {})
        return ThresholdRule(
            threshold=float(r.get("threshold", default.threshold)),
            resolve_below=float(r.get("resolve_below", default.resolve_below)),
            duration_s=float(r.get("duration_s", default.duration_s)),
            severity=str(r.get("severity", default.severity)),
        )

    spike_rule = SpikeRule(**{**d.spikes.__dict__, **spikes,
                              "min_delta": min_delta or d.spikes.min_delta})
    leak_rule = LeakRule(**{**d.memory_leak.__dict__, **raw.get("memory_leak", {})})

    return Config(
        interval_s=float(det.get("interval_s", d.interval_s)),
        history_s=float(det.get("history_s", d.history_s)),
        stale_after_s=float(det.get("stale_after_s", d.stale_after_s)),
        cpu_high=threshold("cpu_high", d.cpu_high),
        mem_high=threshold("mem_high", d.mem_high),
        spikes=spike_rule,
        memory_leak=leak_rule,
    )
