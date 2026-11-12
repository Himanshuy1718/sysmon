#include "sysmon/agent.hpp"

#include <chrono>
#include <iostream>

namespace sysmon {

Agent::Agent(Collector collector, Sink sink, AgentConfig config)
    : collector_(std::move(collector)),
      sink_(std::move(sink)),
      config_(config),
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
    stop_cv_.notify_all();  // wake the collector out of its sleep

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
    // pop() sleeps while the queue is empty and returns nullopt once the
    // queue is closed and fully drained.
    while (auto snapshot = queue_.pop()) {
        try {
            sink_(*snapshot);
            ++sent_;
        } catch (const std::exception& e) {
            ++errors_;
            std::cerr << "[sender] send failed: " << e.what() << "\n";
        }
    }
}

}  // namespace sysmon
