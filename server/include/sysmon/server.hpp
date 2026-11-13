#pragma once
// Collector server.
//
//   accept thread:    waits for new connections; starts one handler thread each
//   handler threads:  read frames from ONE agent, decode, update the registry,
//                     pass snapshots to the MetricsHandler
//   watchdog thread:  every second, marks agents silent for too long as DOWN
//
// Thread-per-connection is simple and fine for tens or hundreds of agents.
// For thousands, you would switch to an event loop (epoll) where one thread
// watches many sockets — a good "how would you scale it?" interview answer.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "sysmon/metrics.hpp"
#include "sysmon/node_registry.hpp"
#include "sysmon/socket.hpp"

namespace sysmon {

struct ServerConfig {
    std::uint16_t port = 9000;  // 0 = any free port (tests)
    std::chrono::milliseconds node_timeout{15000};    // silent this long -> DOWN
    std::chrono::milliseconds watchdog_period{1000};  // how often to check
};

// Receives every snapshot (print now; database in Milestone 4).
// Called from handler threads, possibly several at once: must be thread-safe.
using MetricsHandler = std::function<void(const MetricSnapshot&)>;
// Receives status events such as "node-1 UP" / "node-1 DOWN" (for logging).
using EventHandler = std::function<void(const std::string&)>;

class Server {
public:
    Server(ServerConfig config, MetricsHandler on_metrics, EventHandler on_event = {});
    ~Server();  // stop()

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void start();  // binds the port (throws net::NetError) and starts threads
    void stop();   // closes everything and joins all threads

    std::uint16_t port() const { return port_; }
    const NodeRegistry& registry() const { return registry_; }
    std::size_t active_connections() const;
    std::uint64_t protocol_errors() const { return protocol_errors_.load(); }

private:
    struct Connection {
        std::thread thread;
        std::atomic<bool> finished{false};
    };

    void accept_loop();
    void handle_client(net::Socket socket, std::string address, Connection* self);
    void watchdog_loop();
    void reap_finished();  // joins handler threads that have ended
    void event(const std::string& msg);

    ServerConfig config_;
    MetricsHandler on_metrics_;
    EventHandler on_event_;
    NodeRegistry registry_;

    net::Socket listener_;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    bool started_ = false;

    std::thread accept_thread_;
    std::thread watchdog_thread_;

    mutable std::mutex conn_mutex_;
    std::list<std::unique_ptr<Connection>> connections_;

    std::mutex watchdog_mutex_;
    std::condition_variable watchdog_cv_;

    std::atomic<std::uint64_t> protocol_errors_{0};
};

}  // namespace sysmon
