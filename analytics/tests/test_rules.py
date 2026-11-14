"""Rule tests on synthetic data: no database needed."""

import math
import random

import pytest

from sysmon_analytics.config import Config, LeakRule, SpikeRule, ThresholdRule, load_config
from sysmon_analytics.rules import (Sample, evaluate_host, memory_leak, recovered,
                                    sustained_threshold, top_process, zscore_spike)

CPU = ThresholdRule(threshold=90, resolve_below=80, duration_s=30, severity="warning")


def proc(name, pid, cpu=0.0, rss_kb=0):
    return {"name": name, "pid": pid, "cpu": cpu, "rss_kb": rss_kb}


def series(values, metric="cpu_pct", step=1.0, start=1_000_000.0, **kw):
    """One sample per `step` seconds with `metric` taking the given values."""
    out = []
    for i, v in enumerate(values):
        fields = {"cpu_pct": 5.0, "mem_pct": 40.0}
        fields.update(kw)
        fields[metric] = v
        out.append(Sample(ts=start + i * step, **fields))
    return out


# ------------------------------------------------------------------ sustained threshold
def test_fires_when_every_sample_is_above_threshold():
    s = series([20] * 10 + [95] * 35, top_cpu=(proc("yes", 42, 99.0),))
    f = sustained_threshold(s, "cpu_pct", CPU, "cpu_high")
    assert f is not None
    assert f.kind == "cpu_high" and f.severity == "warning"
    assert "yes" in f.message and "pid 42" in f.message
    assert f.details["top_process"]["name"] == "yes"


def test_one_dip_below_threshold_prevents_firing():
    s = series([95] * 20 + [85] + [95] * 14)  # one sample at 85 inside the 30 s window
    assert sustained_threshold(s, "cpu_pct", CPU, "cpu_high") is None


def test_short_spike_does_not_fire():
    s = series([20] * 60 + [99] * 10)  # only 10 s high
    assert sustained_threshold(s, "cpu_pct", CPU, "cpu_high") is None


def test_needs_enough_history():
    s = series([99] * 5)  # just started, only 5 s of data
    assert sustained_threshold(s, "cpu_pct", CPU, "cpu_high") is None


def test_hysteresis_recovered_only_below_resolve_level():
    assert not recovered(series([95] * 30 + [85] * 20), "cpu_pct", CPU)  # 85: between 80 and 90
    assert recovered(series([95] * 30 + [50] * 20), "cpu_pct", CPU)      # 50: clearly back to normal


# ------------------------------------------------------------------ z-score spike
SPIKE = SpikeRule(z_threshold=4.0, baseline_s=600, recent_s=20, min_baseline_samples=20)


def noisy(n, mean, sd, seed=1):
    rng = random.Random(seed)
    return [max(0.0, rng.gauss(mean, sd)) for _ in range(n)]


def test_spike_detected_against_baseline():
    s = series(noisy(300, 10, 2) + [60] * 20, top_cpu=(proc("ffmpeg", 7, 50.0),))
    f = zscore_spike(s, "cpu_pct", SPIKE)
    assert f is not None and f.kind == "spike_cpu_pct"
    assert f.details["z"] > 4
    assert "ffmpeg" in f.message


def test_normal_noise_is_not_a_spike():
    s = series(noisy(320, 10, 2, seed=3))
    assert zscore_spike(s, "cpu_pct", SPIKE) is None


def test_min_delta_ignores_tiny_jump_on_flat_baseline():
    # Baseline exactly 0.0 (std = 0, so z = infinity), then 0.5: statistically huge, practically nothing.
    s = series([0.0] * 300 + [0.5] * 20)
    assert zscore_spike(s, "cpu_pct", SPIKE) is None


def test_flat_baseline_with_real_jump_fires():
    s = series([0.0] * 300 + [9000.0] * 20, metric="disk_write_kbps")
    f = zscore_spike(s, "disk_write_kbps", SPIKE)
    assert f is not None and f.details["z"] is None  # infinite z reported as None
    assert "disk write" in f.message


def test_spike_relative_to_each_machines_normal():
    # A machine that always writes ~50 MB/s: 52 MB/s is normal for it.
    s = series(noisy(320, 50_000, 3_000, seed=5), metric="disk_write_kbps")
    assert zscore_spike(s, "disk_write_kbps", SPIKE) is None


def test_spike_needs_baseline():
    s = series([10] * 5 + [90] * 20)
    assert zscore_spike(s, "cpu_pct", SPIKE) is None


# ------------------------------------------------------------------ memory leak
LEAK = LeakRule(window_s=300, min_span_s=90, min_slope_pct_per_min=0.5, min_r2=0.9)


def test_steady_growth_is_a_leak_with_time_to_full():
    # +2% per minute, 2 samples per second... use 1 sample/s for 180 s: 40% -> 46%
    values = [40 + 2 * (i / 60) for i in range(180)]
    leaky = lambda i: (proc("java", 99, 1.0, 1_000_000 + i * 2_000), proc("bash", 5, 0.0, 4_000))
    s = [Sample(ts=1_000_000 + i, cpu_pct=5, mem_pct=v, top_mem=leaky(i)) for i, v in enumerate(values)]
    f = memory_leak(s, LEAK)
    assert f is not None
    assert f.details["slope_pct_per_min"] == pytest.approx(2.0, abs=0.01)
    assert f.details["r2"] > 0.99
    # (100 - 45.97) / 2 = ~27 minutes
    assert f.details["minutes_to_full"] == pytest.approx((100 - values[-1]) / 2, abs=0.5)
    assert f.details["growing_process"]["name"] == "java"


def test_leaking_process_that_starts_mid_window_is_named():
    # Regression test for a bug seen in the live demo: the leaking program started
    # after the window began, so it was missing from the first sample and the
    # rule blamed a different process.
    s = []
    for i in range(180):
        top = [proc("postgres", 10, 0.0, 300_000), proc("detector", 20, 0.0, 60_000 + i * 10)]
        if i >= 30:  # leaker appears 30 s into the window and grows 20 MB/s
            top.insert(0, proc("leaky_app", 77, 0.0, (i - 30) * 20 * 1024))
        s.append(Sample(ts=1_000_000 + i, cpu_pct=5, mem_pct=20 + 0.2 * i, top_mem=tuple(top[:5])))
    f = memory_leak(s, LEAK)
    assert f is not None
    assert f.details["growing_process"]["name"] == "leaky_app"
    assert f.details["growing_process"]["grew_mb"] > 2000


def test_ups_and_downs_are_not_a_leak():
    values = [50 + 10 * math.sin(i / 10) for i in range(240)]  # wave: no steady trend
    assert memory_leak(series(values, metric="mem_pct"), LEAK) is None


def test_slow_growth_below_threshold_is_ignored():
    values = [40 + 0.1 * (i / 60) for i in range(240)]  # +0.1%/min
    assert memory_leak(series(values, metric="mem_pct"), LEAK) is None


def test_leak_needs_enough_data():
    values = [40 + 5 * (i / 60) for i in range(30)]  # only 30 s
    assert memory_leak(series(values, metric="mem_pct"), LEAK) is None


# ------------------------------------------------------------------ helpers / combined
def test_top_process_picks_most_frequent_first_place():
    s = series([95] * 4)
    s = [Sample(ts=x.ts, cpu_pct=95, mem_pct=40, top_cpu=(proc(n, i, 80.0),))
         for i, (x, n) in enumerate(zip(s, ["a", "b", "b", "b"]))]
    assert top_process(s)["name"] == "b"
    assert top_process(series([1, 2])) is None  # no process data


def test_evaluate_host_keeps_alert_open_between_thresholds():
    cfg = Config()
    s = series([95] * 40 + [85] * 20)  # dropped to 85: below 90 but not below 80
    r = evaluate_host(s, cfg, open_kinds={"cpu_high"})
    assert "cpu_high" not in r.findings
    assert "cpu_high" in r.keep_open


def test_config_file_loads(tmp_path):
    p = tmp_path / "c.toml"
    p.write_text('[cpu_high]\nthreshold = 70.0\n[spikes.min_delta]\ncpu_pct = 5.0\n')
    cfg = load_config(p)
    assert cfg.cpu_high.threshold == 70.0
    assert cfg.cpu_high.resolve_below == 80.0  # default kept
    assert cfg.spikes.min_delta == {"cpu_pct": 5.0}


def test_shipped_config_is_valid():
    from sysmon_analytics.detector import DEFAULT_CONFIG
    cfg = load_config(DEFAULT_CONFIG)
    assert cfg.cpu_high.resolve_below < cfg.cpu_high.threshold  # hysteresis gap must exist
    assert cfg.mem_high.resolve_below < cfg.mem_high.threshold
