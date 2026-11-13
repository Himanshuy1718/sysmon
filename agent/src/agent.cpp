#include "sysmon/agent.hpp"

#include <chrono>
#include <iostream>

namespace sysmon {

Agent::Agent(Collector collector, Sink sink, AgentConfig config, Heartbeat heartbeat)
    : collector_(std::move(collector)),
      sink_(std::move(sink)),
      config_(config),
      heartbeat_(std::move(heartbeat)),
      queue_(config.queue_capacity) {}

Agent::~Agent() { stop(); }

void Agent::start() {
    std::lock_guard<std::mutex> lock(stop_mutex_);
    if (started_) return;
    started_ = true;
    stop_requested_ = false;
    // Start the consumer first so it is already waiting when data arrives.
    sender_thread_ = std::thread(&Agent::sender_loop, this);
    collector_thread_ = std::thread(&Agent::collector_loop, this);
}

void Agent::stop() {
    {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        if (!started_) return;  // never started: nothing to stop
        stop_requested_ = true;  // second call is harmless: joins below are skipped
    }
    draining_ = true;  // from now on, delivery failures are expected, not errors
    stop_cv_.notify_all();  // wake the collector out of its sleep
    if (on_stopping_) on_stopping_();

    // Shutdown ORDER matters:
    // 1. stop the producer, so nothing new is pushed;
    if (collector_thread_.joinable()) collector_thread_.join();
    // 2. close the queue — the sender drains what is left, then pop() returns nullopt;
    queue_.close();
    // 3. wait for the consumer to finish.
    if (sender_thread_.joinable()) sender_thread_.join();
}

void Agent::collector_loop() {
    const auto interval = std::chrono::duration<double>(config_.interval_sec);
    while (true) {
        {
            // Sleep for one interval, but wake early if stop() is called.
            // wait_for returns true if the predicate became true (stop requested).
            std::unique_lock<std::mutex> lock(stop_mutex_);
            if (stop_cv_.wait_for(lock, interval, [this] { return stop_requested_; })) {
                return;
            }
        }
        try {
            queue_.push(collector_.sample());
            ++collected_;
        } catch (const std::exception& e) {
            // A bad read (e.g. a /proc file briefly unavailable) must not kill the agent.
            ++errors_;
            std::cerr << "[collector] sample failed: " << e.what() << "\n";
        }
    }
}

void Agent::sender_loop() {
    const bool use_heartbeat = heartbeat_ && config_.heartbeat_sec > 0;
    const auto hb_wait = std::chrono::milliseconds(
        static_cast<long>((use_heartbeat ? config_.heartbeat_sec : 1.0) * 1000));

    while (true) {
        // Without heartbeats: pop() sleeps until data arrives.
        // With heartbeats: wait at most hb_wait; if nothing came, send "I'm alive".
        std::optional<MetricSnapshot> snapshot =
            use_heartbeat ? queue_.pop_for(hb_wait) : queue_.pop();

        if (!snapshot) {
            if (queue_.closed()) return;  // closed AND drained: shut down
            try {
                heartbeat_();
                ++heartbeats_;
            } catch (const std::exception& e) {
                ++errors_;
                std::cerr << "[sender] heartbeat failed: " << e.what() << "\n";
            }
            continue;
        }
        try {
            sink_(*snapshot);
            ++sent_;
        } catch (const std::exception& e) {
            if (draining_) {
                // Shutting down and the destination is unreachable: count the
                // leftovers quietly instead of printing one error per snapshot.
                ++unsent_at_shutdown_;
            } else {
                ++errors_;
                std::cerr << "[sender] send failed: " << e.what() << "\n";
            }
        }
    }
}

}  // namespace sysmon
