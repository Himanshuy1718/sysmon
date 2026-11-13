#pragma once
// TcpClient: sends snapshots and heartbeats to the server, and survives
// server restarts and network drops.
//
// Reconnect strategy: EXPONENTIAL BACKOFF — wait 0.5 s, 1 s, 2 s, 4 s ...
// up to a cap (10 s) between attempts. Retrying in a tight loop would flood
// a struggling server; when 1000 agents all lose the server at once, backoff
// stops them from hammering it the moment it comes back.
//
// While the client is retrying, the sender thread is blocked here, so the
// agent's bounded queue fills up and drops its OLDEST snapshots. That is the
// intended behaviour: buffer some data, but never grow without limit.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

#include "sysmon/metrics.hpp"
#include "sysmon/socket.hpp"

namespace sysmon {

struct TcpClientConfig {
    std::string server_host = "127.0.0.1";
    std::uint16_t server_port = 9000;
    int connect_timeout_ms = 3000;
    int send_timeout_ms = 5000;
    double backoff_initial_sec = 0.5;
    double backoff_max_sec = 10.0;
};

class TcpClient {
public:
    TcpClient(TcpClientConfig config, std::string hostname);

    // Both block until sent (reconnecting as needed). They throw only if
    // request_stop() was called and the message could not be delivered.
    void send_snapshot(const MetricSnapshot& s);
    void send_heartbeat();

    // Called from another thread on shutdown: aborts any reconnect wait.
    void request_stop();

    bool connected() const { return connected_.load(); }
    std::uint64_t connects() const { return connects_.load(); }

private:
    void send_payload(const std::string& payload);
    bool connect_with_backoff();  // false if stop requested before success

    TcpClientConfig config_;
    std::string hostname_;
    net::Socket socket_;  // used only by the sender thread

    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> connects_{0};

    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    bool stop_requested_ = false;
};

}  // namespace sysmon
