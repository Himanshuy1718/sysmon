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
| 3 | TCP server + agent client, framing, heartbeat, reconnect | ✅ Done |
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
sudo apt install -y build-essential cmake git libgtest-dev nlohmann-json3-dev
```

**3. Keep the project inside the Linux filesystem** (e.g. `~/projects/sysmon`),
not under `/mnt/c/...`. Builds there are much faster.

**4. Editor:** VS Code on Windows + the *WSL* extension, then run `code .`
from the project folder in Ubuntu. Add the *C/C++* and *CMake Tools* extensions.

## Build, test, run

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure     # 55 tests
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
├── CMakeLists.txt
├── common/                         # shared by agent and server
│   ├── include/sysmon/
│   │   ├── metrics.hpp             # data structs
│   │   ├── socket.hpp              # RAII sockets, framing, connect/listen
│   │   └── protocol.hpp            # JSON messages: hello / metrics / heartbeat
│   └── src/  socket.cpp  protocol.cpp
├── agent/                          # runs on every monitored machine
│   ├── include/sysmon/
│   │   ├── proc_parser.hpp         # /proc parsers
│   │   ├── collector.hpp           # counters -> rates
│   │   ├── thread_safe_queue.hpp   # bounded producer–consumer queue
│   │   ├── agent.hpp               # collector thread + sender thread
│   │   └── tcp_client.hpp          # send to server, reconnect with backoff
│   ├── src/ ...  main.cpp
│   └── tests/                      # 31 tests
└── server/                         # central collector
    ├── include/sysmon/
    │   ├── node_registry.hpp       # which nodes exist, UP/DOWN, last data
    │   └── server.hpp              # accept thread, handler threads, watchdog
    ├── src/ ...  main.cpp
    └── tests/                      # 23 tests: protocol, framing, end-to-end
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

## Milestone 3 — concepts explained

### Try it: one server, two agents (three Ubuntu tabs)
```bash
# Tab 1 — server (marks a node DOWN after 5 s of silence)
./build/server/sysmon_server --port 9000 --timeout 5

# Tab 2 and Tab 3 — two agents pretending to be two machines
./build/agent/sysmon_agent --server 127.0.0.1:9000 --name node-1 --interval 1
./build/agent/sysmon_agent --server 127.0.0.1:9000 --name node-2 --interval 1
```
Then:
- **Ctrl+C node-2.** After 5 s the server prints `node node-2 is DOWN`.
- **Ctrl+C the server, wait, start it again.** node-1 prints
  `connection lost, reconnecting...`, retries, then `connected` by itself.

### How the pieces talk
```
agent: collector thread ─► queue ─► sender thread ─► TcpClient ══TCP══►  server
                                                                           │
                       accept thread ─► one handler thread per agent ─► NodeRegistry
                       watchdog thread: every 1 s, silent nodes ─► DOWN
```

### 1. TCP is a byte stream, so we need framing
TCP guarantees bytes arrive **in order and complete**, but **not in the same
pieces they were sent in**. Two 500-byte messages can arrive as one
1000-byte chunk, or as 3 bytes + 997 bytes. So each message is sent as
```
[4-byte length][payload]
```
The reader calls `read_exact(4)`, learns the length, then `read_exact(length)`.
`read_exact` loops, because `recv()` may return fewer bytes than asked
(a **partial read**). `write_all` does the same for **partial writes**.
- The length is in **network byte order** (big-endian, `htonl`/`ntohl`), so
  machines with different CPUs agree on the number.
- Frames over **1 MiB are rejected**. Otherwise a broken or malicious client
  could say "I'm sending 4 GB" and make the server allocate it.

### 2. The messages (JSON)
```json
{"type":"hello","host":"node-1","version":1}
{"type":"metrics","host":"node-1","data":{"cpu":12.5, ...}}
{"type":"heartbeat","host":"node-1","ts":1794553548970}
```
Every connection starts with `hello`, so the server knows who is talking.
Anything else first, or invalid JSON, closes **that one connection** and
never crashes the server. JSON is easy to read and debug. Protocol Buffers
would be smaller and faster, which is a good "future work" answer.

### 3. Server: thread per connection
- **accept thread:** waits for new agents and starts a handler thread for each.
- **handler thread:** reads frames from its agent and updates the registry.
- **watchdog thread:** marks nodes with no message for `--timeout` seconds as DOWN.

`accept()` and `recv()` would block forever, so they wait with `poll()` in
200 ms slices and check a `stopping_` flag. That's how the server shuts down
cleanly. Finished handler threads are joined ("reaped") by the accept loop.

**Scaling:** thread-per-connection is fine for hundreds of agents. For
thousands you would use **epoll**, an event loop where one thread watches
many sockets and handles whichever is ready. Nginx and Redis work this way.

### 4. Heartbeats: telling "quiet" apart from "dead"
If no metrics arrive, the agent might be dead, or just idle with a long
interval. So when the sender has had nothing to send for `--heartbeat`
seconds (default 5), it sends a tiny heartbeat. It reuses the queue's
`pop_for(timeout)`: a timeout means "nothing to send, so send a heartbeat".
The server uses **any** message as proof of life.

### 5. Reconnecting with exponential backoff
When a send fails, the client closes the socket and retries after
0.5 s, 1 s, 2 s, 4 s … up to 10 s. Retrying in a tight loop would hammer a
server that is trying to recover, and with 1000 agents that could knock it
over again (a "thundering herd"). While reconnecting, the sender thread
waits, the agent's bounded queue keeps buffering, and the oldest snapshots
are dropped if the outage lasts long.

### 6. Socket details worth knowing
| Detail | Why |
|---|---|
| `MSG_NOSIGNAL` on send | Writing to a closed socket raises **SIGPIPE**, which kills the process by default. With this flag, send just returns an error. |
| `SO_REUSEADDR` | Lets the server restart on the same port immediately, instead of waiting ~60 s for the old connection to leave **TIME_WAIT**. |
| `SO_SNDTIMEO` | A send (and on Linux, `connect`) can't block forever on a stuck peer. |
| `TCP_NODELAY` | Disables Nagle's algorithm, which delays small packets to batch them. Our messages are small and we want them sent now. |
| `Socket` RAII class | The file descriptor is closed automatically, even on exceptions, so there are no fd leaks. Move-only, so there are never two owners. |

### 7. Detecting a dead connection before writing (a bug found in testing)
**The bug:** after the server restarted, the first snapshots were lost.
TCP lets you write into a connection the other side has already closed: the
kernel accepts the bytes, and the error only shows up on a later write. So
`send()` "succeeded" while the data went nowhere. The test for this passed
on one machine and failed on WSL, because the timing of the error differs.

**The fix:** when a server process closes or exits, the kernel sends a
**FIN** ("I'm closing"). Before every send, the client checks for it:
`poll()` with timeout 0, then `recv(MSG_PEEK)`. If `recv` returns 0, the
peer has closed, so the client reconnects first and then sends. Our server
never sends data to the agent, so "readable" can only mean "closed".
Result: a server restart loses **zero** snapshots (`sent=8`, server received 8).

**What is still not covered:** if the server **machine** loses power or
the network cable is pulled, no FIN is ever sent, so one snapshot can still
be lost. TCP alone cannot tell you the *application* received your data.
The complete fix is application-level **acknowledgements**: the server
replies `{"ack": seq}` and the agent resends anything unacknowledged after
reconnecting. That gives **at-least-once** delivery, so the server must
ignore duplicates (by host + timestamp). For monitoring, losing one sample in
a power failure is an acceptable trade-off.

### How it's tested (23 tests)
- **Framing on a `socketpair`:** frames written back to back stay separate,
  a frame sent **one byte at a time** is reassembled, oversized frames are
  rejected, and a close mid-frame is an error rather than a clean close.
- Writing to a closed peer returns an error instead of killing the test (SIGPIPE).
- **End-to-end over real localhost TCP:**
  - 8 agents × 25 snapshots at once all arrive
  - the client keeps retrying until a late server starts, without losing its snapshot
  - the client survives a server restart **without losing any snapshot**
  - `peer_closed()` detects a FIN but ignores ordinary pending data
  - a silent node is marked DOWN
  - garbage input drops only that connection
  - `request_stop()` interrupts a long backoff wait

### Questions to be ready for
1. Why do you need framing on top of TCP? What is a partial read?
2. TCP vs UDP: which did you use and why? (TCP: ordered, reliable, and
   losing a metrics message silently is worse than a little overhead.)
3. Walk me through `socket → bind → listen → accept` and `socket → connect`.
4. What is SIGPIPE and how did you avoid it?
5. Why `SO_REUSEADDR`? What is TIME_WAIT?
6. How does the server handle many clients? How would you scale to 10,000? (epoll)
7. How do you detect that an agent died? Why heartbeats?
8. Why exponential backoff? What is a thundering herd?
9. Can data be lost? When? How would you guarantee delivery? (acks, at-least-once, dedupe)
10. What happens if a client sends garbage or claims a 4 GB message?
11. Tell me about a bug you found. (The restart data loss: write "succeeds" on a
    closed connection; fixed by checking for FIN with poll + MSG_PEEK before sending.)

## Next: Milestone 4
Store every snapshot in **PostgreSQL**: schema design for time-series data,
indexes, and a writer that doesn't slow down the network threads.
