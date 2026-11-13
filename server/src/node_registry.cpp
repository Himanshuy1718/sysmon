#include "sysmon/node_registry.hpp"

namespace sysmon {

bool NodeRegistry::touch(const std::string& host, const std::string& address,
                         Clock::time_point now, const MetricSnapshot* snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    NodeState& n = nodes_[host];  // creates the entry on first contact
    const bool was_up = n.up;
    n.host = host;
    n.address = address;
    n.up = true;
    n.last_seen = now;
    ++n.messages;
    if (snapshot) n.last_snapshot = *snapshot;
    return !was_up;
}

std::vector<std::string> NodeRegistry::mark_stale(Clock::time_point now,
                                                  std::chrono::milliseconds timeout) {
    std::vector<std::string> went_down;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [host, n] : nodes_) {
        if (n.up && now - n.last_seen > timeout) {
            n.up = false;
            went_down.push_back(host);
        }
    }
    return went_down;
}

std::vector<NodeState> NodeRegistry::nodes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NodeState> out;
    out.reserve(nodes_.size());
    for (const auto& [host, n] : nodes_) out.push_back(n);
    return out;
}

std::optional<NodeState> NodeRegistry::get(const std::string& host) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = nodes_.find(host);
    if (it == nodes_.end()) return std::nullopt;
    return it->second;
}

}  // namespace sysmon
