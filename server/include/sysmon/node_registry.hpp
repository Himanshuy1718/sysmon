#pragma once
// NodeRegistry: the server's view of every agent it has heard from.
// Many client threads update it at once, and the watchdog thread scans it,
// so every method takes the mutex.

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "sysmon/metrics.hpp"

namespace sysmon {

using Clock = std::chrono::steady_clock;

struct NodeState {
    std::string host;
    std::string address;  // IP:port of the connection
    bool up = false;
    Clock::time_point last_seen{};
    std::uint64_t messages = 0;
    std::optional<MetricSnapshot> last_snapshot;
};

class NodeRegistry {
public:
    // Records any message from `host`. Returns true if the node was DOWN
    // (or unknown) and is now UP — so the caller can log "node is back".
    bool touch(const std::string& host, const std::string& address, Clock::time_point now,
               const MetricSnapshot* snapshot = nullptr);

    // Marks nodes silent for longer than `timeout` as DOWN.
    // Returns only the hosts that changed from UP to DOWN in this call.
    std::vector<std::string> mark_stale(Clock::time_point now, std::chrono::milliseconds timeout);

    std::vector<NodeState> nodes() const;  // copy, safe to use without the lock
    std::optional<NodeState> get(const std::string& host) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, NodeState> nodes_;
};

}  // namespace sysmon
