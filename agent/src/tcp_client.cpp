#include "sysmon/tcp_client.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

#include "sysmon/protocol.hpp"

namespace sysmon {

TcpClient::TcpClient(TcpClientConfig config, std::string hostname)
    : config_(std::move(config)), hostname_(std::move(hostname)) {}

void TcpClient::send_snapshot(const MetricSnapshot& s) {
    send_payload(protocol::encode_metrics(s));
}

void TcpClient::send_heartbeat() {
    send_payload(protocol::encode_heartbeat(hostname_, protocol::now_ms()));
}

void TcpClient::request_stop() {
    {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        stop_requested_ = true;
    }
    stop_cv_.notify_all();
}

void TcpClient::send_payload(const std::string& payload) {
    while (true) {
        // If the server already closed this connection (e.g. it restarted),
        // writing would "succeed" into a dead socket and the data would be
        // silently lost. Detect the FIN first and reconnect before sending.
        if (socket_ && net::peer_closed(socket_.fd())) {
            std::cerr << "[client] server closed the connection, reconnecting...\n";
            socket_.reset();
            connected_ = false;
        }
        if (!socket_ && !connect_with_backoff()) {
            throw std::runtime_error("not connected and shutting down");
        }
        if (net::send_frame(socket_.fd(), payload)) return;

        // Send failed: server closed, crashed or network dropped.
        std::cerr << "[client] connection lost, reconnecting...\n";
        socket_.reset();
        connected_ = false;
        {
            std::lock_guard<std::mutex> lock(stop_mutex_);
            if (stop_requested_) throw std::runtime_error("connection lost during shutdown");
        }
        // Loop: reconnect and RETRY the same payload, so it is not lost.
    }
}

bool TcpClient::connect_with_backoff() {
    double delay = config_.backoff_initial_sec;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(stop_mutex_);
            if (stop_requested_) return false;
        }
        try {
            net::Socket s = net::connect_tcp(config_.server_host, config_.server_port,
                                             config_.connect_timeout_ms);
            net::set_send_timeout(s.fd(), config_.send_timeout_ms);
            // Every new connection starts with a hello so the server knows who we are.
            if (!net::send_frame(s.fd(), protocol::encode_hello(hostname_))) {
                throw net::NetError("hello failed");
            }
            socket_ = std::move(s);
            connected_ = true;
            ++connects_;
            std::cerr << "[client] connected to " << config_.server_host << ":"
                      << config_.server_port << "\n";
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[client] " << e.what() << " - retry in " << delay << "s\n";
        }

        // Sleep `delay`, but wake immediately if request_stop() is called.
        std::unique_lock<std::mutex> lock(stop_mutex_);
        if (stop_cv_.wait_for(lock, std::chrono::duration<double>(delay),
                              [this] { return stop_requested_; })) {
            return false;
        }
        delay = std::min(delay * 2, config_.backoff_max_sec);  // exponential backoff
    }
}

}  // namespace sysmon
