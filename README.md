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
| 2 | Thread-safe queue + collector/sender threads | ✅ Done |
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
ctest --test-dir build --output-on-failure     # 31 tests
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
    │   ├── thread_safe_queue.hpp   # bounded blocking producer–consumer queue
    │   └── agent.hpp               # collector thread + sender thread
    ├── src/
    │   ├── proc_parser.cpp
    │   ├── collector.cpp
    │   ├── agent.cpp
    │   └── main.cpp                # CLI, Ctrl+C handling
    └── tests/
        ├── test_proc_parser.cpp    # parser tests on fake /proc text
        ├── test_collector.cpp      # collector tests on a fake /proc directory
        ├── test_thread_safe_queue.cpp  # incl. 4x4 thread stress test
        └── test_agent.cpp          # threads, shutdown, slow sender
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

## Milestone 2 — concepts explained

### Why two threads?
```
 collector thread                         sender thread
 every 2 s:                               loop:
   snap = collector.sample()                snap = queue.pop()   (sleeps if empty)
   queue.push(snap)     ───► queue ───►     sink(snap)           (print now, TCP in M3)
```
Sending can be slow or stall (network down, server busy). If one thread did
both jobs, a stuck send would also stop measuring, and data would be lost
exactly when something is going wrong. The queue **decouples** the two:
the collector keeps a steady rhythm regardless of the sender.

Try it: a sender that takes 1 s per snapshot, with samples every 0.2 s:
```bash
./build/agent/sysmon_agent --interval 0.2 --queue 3 --sink-delay 1000
# Ctrl+C after a few seconds ->  stopped. collected=14 sent=6 dropped=8
```

### The thread-safe queue (`thread_safe_queue.hpp`)
- **`std::mutex`**: only one thread may touch the `deque` at a time. Without
  it, two threads pushing together can corrupt it (a **data race**).
- **`std::lock_guard` / `std::unique_lock`**: RAII locking. The mutex is
  released automatically when the scope ends, even if an exception is thrown.
- **`std::condition_variable`**: the consumer **sleeps** while the queue is
  empty and is woken by `notify_one()` on push. The alternative,
  `while (empty) {}`, is *busy waiting* and burns a full core.
- **Predicate wait** `cv.wait(lock, [&]{ return !empty || closed; })`:
  threads can wake up for no reason (*spurious wake-ups*), so the condition
  is re-checked every time.
- **Notify after unlocking**: the woken thread can grab the mutex at once
  instead of waking up only to block on it again.
- **Bounded, drop-oldest**: if the server is down for an hour, the agent
  must not eat all RAM. For monitoring, fresh data beats stale data.
- **`close()`**: rejects new pushes and wakes all waiting consumers;
  `pop()` keeps returning queued items, then `nullopt` = "time to exit".

### Graceful shutdown (`Agent::stop()`) — order matters
1. Set `stop_requested_` and `notify` the collector. It is sleeping on a
   `condition_variable` with `wait_for(interval)`, so it wakes **immediately**
   (a plain `sleep_for(60s)` could not be interrupted).
2. `join()` the collector, so nothing new is pushed.
3. `close()` the queue, and the sender drains what is left.
4. `join()` the sender.

Doing step 3 before step 2 could lose snapshots. Forgetting to `join()` a
`std::thread` calls `std::terminate()`, so the destructor calls `stop()` (RAII).

### Other details
- **`std::atomic<uint64_t>`** counters: read from the main thread while
  worker threads update them, with no lock needed.
- **Ctrl+C handler** only sets a `volatile sig_atomic_t` flag. A signal can
  interrupt a thread while it holds a lock, so calling `printf`, `malloc` or
  locking inside a handler can deadlock. The main thread checks the flag.
- **Exceptions in a thread**: an uncaught exception kills the whole process,
  so each loop catches errors, counts them and keeps going.

### How the concurrency is tested
- Blocking: `pop()` must actually wait until another thread pushes.
- `close()` must wake a blocked consumer (otherwise the test hangs).
- **Stress test**: 4 producers × 20,000 items, 4 consumers. Every item must
  arrive exactly once (sum check: 1+2+…+n = n(n+1)/2).
- Slow sink: collection continues, the queue drops old items.
- **ThreadSanitizer** build detects data races at runtime:
  ```bash
  cmake -S . -B build-tsan -DSYSMON_TSAN=ON && cmake --build build-tsan -j
  ./build-tsan/agent/agent_tests        # must print no "WARNING: ThreadSanitizer"
  ```

### Questions to be ready for
1. What is the producer–consumer problem? How did you solve it?
2. Why a condition variable instead of a loop that checks the queue?
3. What is a spurious wake-up and how do you handle it?
4. What happens when the queue is full? Why drop the oldest, not the newest?
5. Walk me through shutdown. Why does the order matter?
6. Mutex vs atomic: when do you use which?
7. Why can't you call `printf` in a signal handler?
8. How do you test multithreaded code? What is ThreadSanitizer?
9. What is a deadlock? Could this code deadlock? (No: there is only one lock
   per object, and no lock is held while calling the sink.)

## Next: Milestone 3
Replace the print sink with a **TCP client** in the agent and build the
**collector server**: message framing, multiple clients, heartbeat and
auto-reconnect.
