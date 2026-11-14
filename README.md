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
| 4 | PostgreSQL storage | ✅ Done |
| 5 | Anomaly detector + alerts (Python) | ✅ Done |
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
sudo apt install -y build-essential cmake git libgtest-dev nlohmann-json3-dev \
                    postgresql libpq-dev pkg-config
```

**3. Keep the project inside the Linux filesystem** (e.g. `~/projects/sysmon`),
not under `/mnt/c/...`. Builds there are much faster.

**4. Editor:** VS Code on Windows + the *WSL* extension, then run `code .`
from the project folder in Ubuntu. Add the *C/C++* and *CMake Tools* extensions.

## Build, test, run

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure     # 68 tests (7 skipped without a DB)
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
│   │   ├── thread_safe_queue.hpp   # bounded producer–consumer queue (agent + server)
│   │   ├── socket.hpp              # RAII sockets, framing, connect/listen
│   │   └── protocol.hpp            # JSON messages: hello / metrics / heartbeat
│   └── src/  socket.cpp  protocol.cpp
├── agent/                          # runs on every monitored machine
│   ├── include/sysmon/
│   │   ├── proc_parser.hpp         # /proc parsers
│   │   ├── collector.hpp           # counters -> rates
│   │   ├── agent.hpp               # collector thread + sender thread
│   │   └── tcp_client.hpp          # send to server, reconnect with backoff
│   ├── src/ ...  main.cpp
│   └── tests/                      # 32 tests
├── server/                         # central collector
│   ├── include/sysmon/
│   │   ├── node_registry.hpp       # which nodes exist, UP/DOWN, last data
│   │   ├── server.hpp              # accept thread, handler threads, watchdog
│   │   ├── store.hpp               # storage interface (fake in tests, PostgreSQL for real)
│   │   ├── db_writer.hpp           # background batching writer with retries
│   │   └── pg_store.hpp            # PostgreSQL via libpq: transactions, prepared statements
│   ├── sql/schema.sql              # tables + indexes (embedded into the binary)
│   ├── src/ ...  main.cpp
│   └── tests/                      # 36 tests: protocol, framing, end-to-end, storage
├── analytics/                      # Python: anomaly detection (Milestone 5)
│   ├── config.toml                 # every threshold, tunable without code changes
│   ├── sql/alerts.sql              # alerts table + "one open alert per kind" index
│   ├── sysmon_analytics/
│   │   ├── rules.py                # pure detection functions (threshold, z-score, leak)
│   │   ├── store.py                # PostgreSQL access (psycopg 3)
│   │   └── detector.py             # the service loop: detect → open/refresh/resolve
│   └── tests/                      # 26 pytest tests (20 synthetic, 6 real DB)
└── scripts/
    ├── setup_db.sh                 # create the sysmon database user and database
    ├── queries.sql                 # example SQL queries over the collected data
    ├── stress_cpu.sh               # load every core (triggers cpu_high)
    └── leak_memory.py              # simulated memory leak (triggers memory_leak)
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

## Milestone 4 — concepts explained

### Setup (once)
```bash
sudo apt install -y postgresql libpq-dev pkg-config
./scripts/setup_db.sh            # creates user "sysmon" and database "sysmon"
```
WSL doesn't always start services automatically. If the database isn't
running after a reboot: `sudo service postgresql start`.
To free its ~30 MB of RAM when you're not using it: `sudo service postgresql stop`.

### Try it
```bash
# Tab 1: server writing to the database
./build/server/sysmon_server --port 9000 --timeout 5 --quiet \
    --db postgresql://sysmon:sysmon@localhost:5432/sysmon

# Tab 2 and Tab 3: agents as before
./build/agent/sysmon_agent --server 127.0.0.1:9000 --name node-1 --interval 1
./build/agent/sysmon_agent --server 127.0.0.1:9000 --name node-2 --interval 1

# Tab 4: look at the data
psql postgresql://sysmon:sysmon@localhost:5432/sysmon -f scripts/queries.sql
```
**Resilience test:** while everything runs, `sudo service postgresql stop`,
wait 10 s, then `sudo service postgresql start`. The server logs retries,
then catches up. Query 7 in `queries.sql` (gaps) shows **no missing data**.

### Schema (`server/sql/schema.sql`)
| Table | One row per | Notes |
|---|---|---|
| `nodes` | machine | current status UP/DOWN, last_seen |
| `metrics` | snapshot | the time series; top processes as **JSONB** |
| `node_events` | UP/DOWN change | history of outages |

- **Foreign keys** (`metrics.host → nodes.host`): the database itself refuses
  a metric for an unknown node.
- **CHECK constraints** (`status IN ('UP','DOWN')`): invalid values are
  impossible to store.
- **Index `(host, ts DESC)`**: almost every query asks for "node X in a
  time window, newest first". With the index, PostgreSQL jumps straight to
  those rows instead of scanning millions (`EXPLAIN` shows *Index Scan*).
  Indexes cost space and slow inserts slightly, so only add the ones queries need.
- **JSONB for top processes**: a variable-length list always read together
  with its snapshot, so a separate table would add joins for no benefit.
  It's still queryable: `top_cpu->0->>'name'`.
- **`TIMESTAMPTZ`**: timestamps with time zone, stored as UTC.
- The schema file is **embedded into the binary at build time** (CMake
  `configure_file`) and applied on startup with `CREATE ... IF NOT EXISTS`,
  so it's safe to run repeatedly.

### Getting data in without slowing the network (`DbWriter`)
```
handler threads ──submit()──► [bounded queue] ──► writer thread ──► PostgreSQL
                  (instant)                       batch + transaction + retry
```
The same producer–consumer pattern as the agent, reusing the same
`ThreadSafeQueue`, now moved to `common/`.
1. **Decoupling:** handler threads only enqueue, which takes microseconds.
   A slow or dead database never stalls agent connections.
2. **Batching:** up to 500 rows per transaction. Each `COMMIT` waits for
   the disk, so 1 commit for 500 rows is far faster than 500 commits.
   A batch is written when it's **full** or after **1 s**, whichever
   comes first, so data is never delayed long.
3. **Retries:** if a write fails, the writer waits 2 s and retries the
   **same batch**. Meanwhile new data waits in the queue (bounded at
   50,000 items; the oldest are dropped beyond that).

### Transactions make retries safe
Each batch is one transaction: `BEGIN … COMMIT`. If anything fails
midway, `ROLLBACK` undoes everything, so the database never holds half a
batch, and retrying cannot create duplicates. This is **atomicity**, the
"A" in **ACID**. Tested by adding a constraint that one row in the middle
of a batch violates: none of the batch's rows are saved.

### Prepared statements and SQL injection
Inserts use `PQprepare` + `PQexecPrepared` with `$1, $2 …` placeholders:
- The SQL is parsed and planned **once per connection**, not once per row.
- **Values are sent separately from the SQL text.** A node named
  `x'); DROP TABLE metrics; --` is stored as plain text and never runs as
  SQL. Tested.
Building SQL with string concatenation would be an injection hole, because
host names come from the network.

### A bug found in testing: prepare order
The first version prepared its statements right after connecting, before
creating the tables. PostgreSQL checks that tables exist **at prepare
time**, so on a brand-new database every insert failed. Fixed: connect,
create schema, then prepare. Prepared statements belong to one connection,
so they are prepared again after every reconnect.

### Other details
- **libpq RAII:** results are wrapped in `unique_ptr` with a custom deleter
  (`PQclear`), so there's no leak even when an exception is thrown.
- **Interface + fake for tests:** `DbWriter` depends on the abstract
  `MetricStore`. Unit tests pass a fake in-memory store to test batching
  and retry logic with no database. `PgStore` is tested separately against
  real PostgreSQL. This is **dependency inversion**.
- Database tests run in a **throw-away PostgreSQL schema** (namespace) per
  test, dropped afterwards, so they never touch real data. They run only
  when `SYSMON_TEST_DB` is set:
  ```bash
  SYSMON_TEST_DB=postgresql://sysmon:sysmon@localhost:5432/sysmon ctest --test-dir build
  ```
- **Shutdown order:** stop the network server first (no new data), then
  stop the writer, which flushes everything queued. The log prints
  `db final: written=… lost=…`.

### Measured
Two agents at 0.5 s intervals, PostgreSQL stopped for 7 s mid-run:
`agents sent 75 snapshots + 3 events = 78; db final: written=78 lost=0`,
and the largest gap between stored samples was 0.5 s.

### Questions to be ready for
1. Why PostgreSQL? Why not write to a file? (Queries, indexes, concurrent
   readers, constraints, transactions.) When would you choose a
   time-series database like TimescaleDB or InfluxDB?
2. Explain your schema. Why is top_cpu JSONB and not a separate table?
3. What index did you add and why? How do you check that a query uses it? (`EXPLAIN`)
4. Why batch inserts? What is the trade-off? (Throughput vs latency, bounded by the 1 s flush.)
5. What is a transaction? What does ACID mean? How does it make retries safe?
6. What is SQL injection and how did you prevent it?
7. What happens when the database goes down? Can data be lost? (Buffered and
   retried; lost only if the outage outlasts the 50,000-item queue.)
8. Why not insert directly from the network threads?
9. The table grows forever. What would you do? (Retention `DELETE`,
   partitioning by time, downsampling old data to 1-minute averages.)
10. How did you test database code? (Interface + fake, throw-away schema, real DB tests.)

## Milestone 5 — concepts explained

### Setup (once)
The detector is Python. It gets its **own virtual environment** inside the
project, so it never touches conda or your thesis environments.
```bash
sudo apt install -y python3-venv python3-pip
conda deactivate                      # if (base) is showing
cd ~/projects/sysmon/analytics
python3 -m venv .venv
source .venv/bin/activate             # prompt now starts with (.venv)
pip install -r requirements.txt
pytest                                # 26 tests (6 skipped unless SYSMON_TEST_DB is set)
```
Each new terminal needs `source ~/projects/sysmon/analytics/.venv/bin/activate`
before running the detector. `deactivate` leaves the environment.

### Try it (4 tabs)
```bash
# Tab 1: server with database
./build/server/sysmon_server --port 9000 --timeout 5 --quiet --db $SYSMON_DB
# Tab 2: one agent
./build/agent/sysmon_agent --server 127.0.0.1:9000 --name node-1 --interval 1
# Tab 3: the detector
cd analytics && source .venv/bin/activate && python -m sysmon_analytics.detector
# Tab 4: wait ~30 s for a baseline, then cause trouble:
./scripts/stress_cpu.sh 60                  # → spike_cpu_pct, then cpu_high, then RESOLVED
python3 scripts/leak_memory.py 20 2500      # → memory_leak (after ~80 s), names the process
# Ctrl+C the agent in Tab 2                  → node_down (critical)
psql $SYSMON_DB -c "\x" -f scripts/queries.sql   # query 10 lists the alerts
```

### Measured (live run)
| What I did | What the detector reported |
|---|---|
| loaded all cores | after 10 s: `spike_cpu_pct` "50.4% vs normal 1.1% (z = 28.7); top process: yes" |
| kept them loaded | after 40 s: `cpu_high` "above 90% for 29 s, avg 100%; top process: yes (pid 319)" |
| stopped the load | `cpu_high` RESOLVED |
| leaked 20 MB/s | after ~80 s: `memory_leak` "+13.45%/min (R² = 0.98), full in ~6 min; fastest-growing process: python3 (+1550 MB)" |
| killed an agent | `node_down` (critical) |

### Architecture
```
PostgreSQL (metrics, nodes) ──every 10 s──► detector.py
                                              │ fetch last 15 min per host
                                              │ rules.py: evaluate_host()   (pure functions)
                                              ▼
                            alerts table: OPEN / refresh / RESOLVE
```
The detector is a **separate process** from the C++ server. Analysis can be
written in Python (numpy, quick to change), restarted or scaled on its own,
and a crash in it never affects data collection. The database is the contract
between the two.

### The four rules (`rules.py`)
**1. Sustained threshold (`cpu_high`, `mem_high`)**
Fires only if **every** sample in the last 30 s is above 90%. Using "every"
rather than "average" means a 2-second blip never fires it.

**2. Spike, by z-score (`spike_<metric>`)**
```
baseline = this machine's last 10 minutes (excluding the last 20 s)
z = (mean(last 20 s) − mean(baseline)) / std(baseline)
```
z = "how many standard deviations above normal". z ≥ 4 almost never happens
by chance. The baseline is **per machine**, so a database server that always
writes 50 MB/s isn't flagged for writing 52 MB/s.
- `min_delta`: on an idle machine, CPU going from 0.1% to 0.5% gives a huge z
  but means nothing, so a jump must also be big in absolute terms.
- `std = 0` (perfectly flat baseline): division by zero is avoided, treated as z = ∞.
- A spike alert closes once the new level becomes the norm. Spikes detect
  **changes**, thresholds detect **states**, so for a sustained load,
  `cpu_high` takes over.

**3. Memory leak by linear regression (`memory_leak`)**
Fit a straight line to memory % over time (`numpy.polyfit`, least squares):
- **slope** = growth in % per minute
- **R²** (0 to 1) = how well the line fits. Near 1 means steady growth, which is
  what a leak looks like. Normal usage goes up and down, so R² is low.
- Time until memory is full = (100 − current%) / slope.
- Culprit = the process whose memory grew most since it **first appeared** in
  the window.

**4. Node down (`node_down`)**
The C++ server's watchdog already marks silent nodes `DOWN`. The detector
turns that into a critical alert and resolves it when the node is back.

### Alert lifecycle: open → refresh → resolve
- **Deduplication:** a problem that lasts 10 minutes is **one** alert updated 60
  times, not 60 alerts. The database enforces it with a **partial unique index**:
  ```sql
  CREATE UNIQUE INDEX ... ON alerts (host, kind) WHERE resolved_at IS NULL;
  ```
  Even two detectors running at once can't create duplicates.
  `INSERT ... ON CONFLICT (host, kind) WHERE resolved_at IS NULL DO UPDATE`
  opens or refreshes in one atomic statement. `RETURNING (xmax = 0)` tells
  whether the row was newly inserted.
- **Hysteresis:** fire at 90%, but resolve only below 80%. Without the gap, a
  value hovering around 90% (89, 91, 89 …) would open and close an alert every
  10 seconds ("flapping"). That kind of noise teaches people to ignore alerts,
  known as **alert fatigue**.
- **Stale hosts:** if a host sent no data for 60 s, its open alerts are left
  alone. We can't know it recovered, and `node_down` already reports the silence.

### A bug found in the live demo
The first leak alert blamed `python (+2 MB)`, the wrong process. The real
leaker grew by 2.4 GB, but it **started after the window began**, so it
wasn't in the first sample's top-5 list. The code compared only processes
present in both the first and last samples. **Fix:** measure each process's
growth from the first sample where it appears. A regression test reproduces
the exact situation.

### Testing
- **Rules are pure functions**, tested with synthetic data where the answer is
  known: a steady ramp of +2%/min must give slope 2.0 and R² > 0.99; a sine wave
  must not be a leak; random noise must not be a spike; one dip below 90%
  must prevent `cpu_high`.
- **Database tests** (`SYSMON_TEST_DB`) run in a throw-away schema: open with
  the culprit named, no duplicates over repeated passes, the database rejecting
  a second open alert, hysteresis keeping an alert open at 85%, node down and
  back up, stale hosts left alone.

### Note on the demo
Both demo agents run on the **same laptop**, so they see the same CPU and
memory, and both raise the same alerts. On real separate machines, each
agent would see only its own problems.

### Questions to be ready for
1. What rules did you implement and why those? (Threshold, z-score spike, trend/leak, liveness.)
2. Why "every sample above 90% for 30 s" and not "average above 90%"?
3. Explain the z-score. Why per-machine baselines? What breaks when std = 0?
4. How do you detect a memory leak? What does R² tell you?
5. What is hysteresis? What is alert fatigue and how do you reduce it?
6. How do you avoid duplicate alerts? (Partial unique index + ON CONFLICT upsert.)
7. Why a separate Python process instead of doing it in the C++ server?
8. What are the limits? (Fixed thresholds; z-score assumes a roughly stable
   baseline; daily patterns like nightly backups would need seasonal baselines;
   could add ML methods like Isolation Forest.)
9. Tell me about a bug you found. (The wrong leak culprit.)

## Next: Milestone 6
A **REST API** (FastAPI): `GET /nodes`, `GET /metrics/{host}`, `GET /alerts`,
so dashboards and the AI assistant can read the data over HTTP.
