#include "sysmon/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <condition_variable>

#include "sysmon/protocol.hpp"

namespace sysmon {

Server::Server(ServerConfig config, MetricsHandler on_metrics, EventHandler on_event)
    : config_(config), on_metrics_(std::move(on_metrics)), on_event_(std::move(on_event)) {}

Server::~Server() { stop(); }

void Server::event(const std::string& msg) {
    if (on_event_) on_event_(msg);
}

void Server::start() {
    if (started_) return;
    listener_ = net::listen_tcp(config_.port);
    port_ = net::local_port(listener_.fd());
    started_ = true;
    stopping_ = false;
    accept_thread_ = std::thread(&Server::accept_loop, this);
    watchdog_thread_ = std::thread(&Server::watchdog_loop, this);
    event("listening on port " + std::to_string(port_));
}

void Server::stop() {
    if (!started_) return;
    started_ = false;
    stopping_ = true;  // every loop polls this flag within ~200 ms
    {
        std::lock_guard<std::mutex> lock(watchdog_mutex_);
    }
    watchdog_cv_.notify_all();

    if (accept_thread_.joinable()) accept_thread_.join();
    if (watchdog_thread_.joinable()) watchdog_thread_.join();

    // Handler threads see stopping_ in their read loop and exit.
    std::list<std::unique_ptr<Connection>> conns;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        conns.swap(connections_);
    }
    for (auto& c : conns) {
        if (c->thread.joinable()) c->thread.join();
    }
    listener_.reset();
    event("server stopped");
}

std::size_t Server::active_connections() const {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    std::size_t n = 0;
    for (const auto& c : connections_) n += c->finished ? 0 : 1;
    return n;
}

void Server::reap_finished() {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    for (auto it = connections_.begin(); it != connections_.end();) {
        if ((*it)->finished) {
            (*it)->thread.join();  // already returned, so this is instant
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
}

void Server::accept_loop() {
    while (!stopping_) {
        // accept() would block forever; poll with a timeout so we can see stopping_.
        pollfd pfd{listener_.fd(), POLLIN, 0};
        int r = ::poll(&pfd, 1, 200);
        reap_finished();  // clean up handler threads of disconnected agents
        if (r <= 0) continue;  // timeout (or EINTR): check the flag again

        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        int fd = ::accept(listener_.fd(), reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd < 0) continue;

        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        std::string address = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));

        // One thread per connection. The Connection object lives in the list
        // so the thread can flag itself finished, and stop() can join it.
        auto conn = std::make_unique<Connection>();
        Connection* raw = conn.get();
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            connections_.push_back(std::move(conn));
            raw->thread = std::thread(&Server::handle_client, this, net::Socket(fd), address, raw);
        }
    }
}

void Server::handle_client(net::Socket socket, std::string address, Connection* self) {
    std::string host;  // known after the hello message
    std::string payload;

    while (!stopping_) {
        net::ReadStatus st = net::recv_frame(socket.fd(), payload, &stopping_);
        if (st == net::ReadStatus::Stopped) break;
        if (st == net::ReadStatus::Closed) {
            event("connection closed: " + (host.empty() ? address : host));
            break;
        }
        if (st == net::ReadStatus::Error) {
            ++protocol_errors_;
            event("bad frame or socket error from " + address + ", dropping connection");
            break;
        }

        try {
            protocol::Message m = protocol::decode(payload);
            if (host.empty() && m.type != protocol::MessageType::Hello) {
                throw protocol::ProtocolError("first message must be hello");
            }
            if (m.type == protocol::MessageType::Hello) {
                host = m.host;
                event("hello from " + host + " (" + address + ")");
            }
            const MetricSnapshot* snap =
                m.type == protocol::MessageType::Metrics ? &m.snapshot : nullptr;
            if (registry_.touch(host, address, Clock::now(), snap)) {
                event("node " + host + " is UP");
                if (on_status_) on_status_(host, address, true, protocol::now_ms());
            }
            if (snap && on_metrics_) on_metrics_(*snap);
        } catch (const std::exception& e) {
            // A broken message ends only this connection, never the server.
            ++protocol_errors_;
            event("protocol error from " + address + ": " + e.what());
            break;
        }
    }
    self->finished = true;  // socket closes here (RAII); accept loop will join us
}

void Server::watchdog_loop() {
    std::unique_lock<std::mutex> lock(watchdog_mutex_);
    while (!stopping_) {
        watchdog_cv_.wait_for(lock, config_.watchdog_period, [this] { return stopping_.load(); });
        if (stopping_) break;
        for (const auto& host : registry_.mark_stale(Clock::now(), config_.node_timeout)) {
            if (on_status_) on_status_(host, "", false, protocol::now_ms());
            event("node " + host + " is DOWN (no data for " +
                  std::to_string(config_.node_timeout.count() / 1000.0).substr(0, 4) + " s)");
        }
    }
}

}  // namespace sysmon
