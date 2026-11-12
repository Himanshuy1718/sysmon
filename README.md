# SysMon — AI-Powered System Health Monitor & Diagnostics Agent

A lightweight distributed monitoring system. C++ agents collect Linux system
metrics from `/proc` and stream them to a central server, which stores them,
detects anomalies, and answers plain-English questions about incidents using an
LLM with retrieval-augmented generation (RAG).

```
 [Agent: node-1] ─┐
 [Agent: node-2] ─┼──TCP──► [Collector Server] ──► [PostgreSQL]
 [Agent: node-3] ─┘              │                      │
                                 ▼                      ▼
                        [Anomaly Detector] ──► [Alerts table]
                                                        │
                   User question ──► [RAG Assistant: retrieve → LLM] ──► Answer
```

## Status

| # | Milestone | Status |
|---|-----------|--------|
| 1 | `/proc` parsers, metric collector, unit tests | ✅ Done |
| 2 | Thread-safe queue + collector/sender threads | ⏳ |
| 3 | TCP server + agent client, framing, heartbeat, reconnect | ⏳ |
| 4 | PostgreSQL storage | ⏳ |
| 5 | Anomaly detector + alerts | ⏳ |
| 6 | FastAPI REST API | ⏳ |
| 7 | RAG diagnostics assistant (Ollama) | ⏳ |
| 8 | Docker Compose + GitHub Actions CI | ⏳ |

---

## Setup (Windows + WSL2)

`/proc` and POSIX sockets exist only on Linux, so everything runs inside WSL.

**1. Install WSL with Ubuntu** (PowerShell as Administrator, then reboot):
```powershell
wsl --install -d Ubuntu-24.04
```

**2. Inside Ubuntu, install the toolchain:**
```bash
sudo apt update
sudo apt install -y build-essential cmake git libgtest-dev
```

**3. Keep the project inside the Linux filesystem** (e.g. `~/projects/sysmon`),
not under `/mnt/c/...`. Builds there are much faster.

**4. Editor:** VS Code on Windows + the *WSL* extension, then run `code .`
from the project folder in Ubuntu. Add the *C/C++* and *CMake Tools* extensions.

## Build, test, run

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure     # 19 tests
./build/agent/sysmon_agent --interval 2         # Ctrl+C to stop
./build/agent/sysmon_agent --interval 1 --count 5
```

Sample output:
```
[my-laptop]  t=1794506600421
  CPU     12.4 %      load 0.29 0.07 0.02
  MEM     41.3 %      3300 / 7990 MB
  DISK  read      0.0 kB/s   write     48.0 kB/s
  NET   rx        2.1 kB/s   tx         0.9 kB/s
  Top CPU:  python3(812) 35.0%  sysmon_agent(904) 0.5% ...
  Top MEM:  python3(812) 410MB  code-server(77) 180MB ...
```

To watch it react, open a second terminal and run `yes > /dev/null` (CPU),
or `dd if=/dev/zero of=/tmp/big bs=1M count=500` (disk). Remove `/tmp/big` after.

## Project structure

```
sysmon/
├── CMakeLists.txt                  # top-level build
└── agent/
    ├── CMakeLists.txt              # library + agent binary + tests
    ├── include/sysmon/
    │   ├── metrics.hpp             # data structs (raw counters + snapshot)
    │   ├── proc_parser.hpp         # /proc parsers + ProcReader
    │   └── collector.hpp           # turns counters into rates
    ├── src/
    │   ├── proc_parser.cpp
    │   ├── collector.cpp
    │   └── main.cpp                # CLI: sample and print
    └── tests/
        ├── test_proc_parser.cpp    # parser tests on fake /proc text
        └── test_collector.cpp      # collector tests on a fake /proc directory
```

---

## Milestone 1 — concepts explained

### `/proc` is a virtual filesystem
Files under `/proc` are not on disk. When you read one, the kernel generates
the text on the spot from its internal data. `top`, `htop` and `free` all work
by reading these files.

| File | What we read |
|------|--------------|
| `/proc/stat` | CPU time spent in each state, in *ticks* since boot |
| `/proc/meminfo` | `MemTotal`, `MemAvailable` (kB) |
| `/proc/diskstats` | sectors read/written per disk since boot |
| `/proc/net/dev` | bytes received/sent per interface since boot |
| `/proc/loadavg` | 1/5/15-minute load averages |
| `/proc/[pid]/stat` | per-process CPU ticks (`utime`+`stime`) and memory (`rss`) |

### Counters vs. rates (the key idea)
Most values are **cumulative counters**: "total CPU ticks since boot" or
"total bytes sent since boot". One reading says nothing about *now*. So the
`Collector` keeps the previous reading and computes

```
rate = (current − previous) / elapsed_time
```

That's why the agent takes a baseline reading at start-up and the first
printout appears only after one interval.

### How CPU % is computed
`/proc/stat` line: `cpu user nice system idle iowait irq softirq steal`
```
total = sum of all fields          idle = idle + iowait
CPU%  = (Δtotal − Δidle) / Δtotal × 100
```
`iowait` counts as idle, because the CPU is free while it waits for disk.

Per-process CPU % = `Δ(utime+stime) / (elapsed × CLK_TCK) × 100`, where
`CLK_TCK` (usually 100) is ticks per second. As in `top`, 100% means one full core.

### Memory used
`used = MemTotal − MemAvailable`. Don't use `MemFree`: Linux uses spare
RAM as disk cache, so `MemFree` looks scarily low even when memory is fine.
`MemAvailable` already counts reclaimable cache as available.

### Avoiding double counting on disks
`/proc/diskstats` lists both `sda` and its partitions `sda1`, `sda2`.
Adding them all would count the same I/O twice. `is_physical_disk()` skips
any device whose name starts with another device's name, plus virtual devices
(`loop`, `ram`, `dm-`). 1 sector = 512 bytes.

### Tricky parse: process names
`/proc/[pid]/stat` looks like `1234 (Web Content) S 1 ...`. The name can contain
spaces and even `)`, so splitting on spaces breaks. The parser takes the text
between the **first `(`** and the **last `)`**.

### Design choices worth mentioning in interviews
- **Parsers take `std::istream`, not file paths.** They are pure functions, so
  tests feed in fake text without touching real files.
- **`ProcReader` takes a root path** (`/proc` by default). Tests point it at a
  temporary fake directory to check exact numbers.
- **`sample_with_elapsed()`** lets tests set the time interval, so they don't
  depend on real timing and never flake.
- **Processes may exit while we read them.** `parse_pid_stat` returns
  `std::optional` and failures are skipped instead of crashing the agent.
- **Counter resets** (e.g. an interface restarts) give `cur < prev`; we
  report 0 instead of a huge bogus rate.

### Questions to be ready for
1. How do you calculate CPU usage from `/proc/stat`? Why is `iowait` treated as idle?
2. Why `MemAvailable` and not `MemFree`?
3. Why do you need two readings to report network speed?
4. How did you make code that reads `/proc` unit-testable?
5. What happens if a process exits while you are reading its stat file?
6. How would you avoid counting partition I/O twice?

## Next: Milestone 2
Split collection and sending into two threads connected by a thread-safe
queue (`std::mutex` + `std::condition_variable`), i.e. the producer–consumer pattern.
