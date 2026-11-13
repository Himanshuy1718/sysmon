#pragma once
// Agent = two threads joined by a queue.
//
//   collector thread                         sender thread
//   ----------------                         -------------
//   every interval:                          loop:
//     snapshot = collector.sample()            snapshot = queue.pop()  (sleeps if empty)
//     queue.push(snapshot)   ───queue───►      sink(snapshot)          (print now, TCP in M3)
//
// Why two threads? Sending can be slow or stall (network down, server busy).
// If one thread did both, a stalled send would also stop measurement and we
// would lose data exactly when something is going wrong. With a queue in the
// middle, collection keeps its steady rhythm no matter what the sender does.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "sysmon/collector.hpp"
#include "sysmon/metrics.hpp"
#include "sysmon/thread_safe_queue.hpp"

namespace sysmon {

// Where snapshots go: print to terminal, or TCP to the server.
using Sink = std::function<void(const MetricSnapshot&)>;
// Called by the sender thread when nothing was sent for `heartbeat_sec`.
using Heartbeat = std::function<void()>;

struct AgentConfig {
    double interval_sec = 2.0;      // time between samples
    std::size_t queue_capacity = 100;  // snapshots buffered before dropping oldest
    double heartbeat_sec = 0;       // 0 = no heartbeats
};

class Agent {
public:
    Agent(Collector collector, Sink sink, AgentConfig config = {}, Heartbeat heartbeat = {});
    ~Agent();  // calls stop() — threads are never left running (RAII)

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    void start();
    // Stops collecting, lets the sender finish what is queued, joins both
    // threads. Safe to call more than once.
    void stop();

    // Optional: called at the start of stop(), after the agent has marked
    // itself as shutting down (used to abort the TCP client's reconnect wait).
    void set_on_stopping(std::function<void()> hook) { on_stopping_ = std::move(hook); }

    std::uint64_t collected() const { return collected_.load(); }
    std::uint64_t sent() const { return sent_.load(); }
    std::uint64_t dropped() const { return queue_.dropped(); }
    std::uint64_t errors() const { return errors_.load(); }

private:
    void collector_loop();
    void sender_loop();

    Collector collector_;
    Sink sink_;
    AgentConfig config_;
    Heartbeat heartbeat_;
    std::function<void()> on_stopping_;
    ThreadSafeQueue<MetricSnapshot> queue_;

    std::thread collector_thread_;
    std::thread sender_thread_;

    // Lets stop() interrupt the collector's sleep immediately instead of
    // waiting up to a full interval.
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    bool stop_requested_ = false;
    bool started_ = false;

    // Counters read by other threads -> atomic, no lock needed.
    std::atomic<std::uint64_t> collected_{0};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<std::uint64_t> heartbeats_{0};
    std::atomic<std::uint64_t> unsent_at_shutdown_{0};
    std::atomic<bool> draining_{false};  // stop() in progress: expected failures are not logged

public:
    std::uint64_t heartbeats() const { return heartbeats_.load(); }
    // Snapshots still queued at shutdown that could not be delivered.
    std::uint64_t unsent_at_shutdown() const { return unsent_at_shutdown_.load(); }
};

}  // namespace sysmon
