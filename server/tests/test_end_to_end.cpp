// End-to-end: real TCP over localhost between Server and the agent's TcpClient.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "sysmon/protocol.hpp"
#include "sysmon/server.hpp"
#include "sysmon/socket.hpp"
#include "sysmon/tcp_client.hpp"

using namespace std::chrono_literals;
using namespace sysmon;

namespace {

template <typename Cond>
bool wait_until(Cond cond, std::chrono::milliseconds timeout = 3000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return cond();
}

MetricSnapshot snap(const std::string& host, double cpu) {
    MetricSnapshot s;
    s.host = host;
    s.timestamp_ms = protocol::now_ms();
    s.cpu_percent = cpu;
    return s;
}

// Collects everything the server receives, thread-safely.
struct Received {
    std::mutex m;
    std::vector<MetricSnapshot> items;
    void add(const MetricSnapshot& s) {
        std::lock_guard<std::mutex> lock(m);
        items.push_back(s);
    }
    std::size_t size() {
        std::lock_guard<std::mutex> lock(m);
        return items.size();
    }
};

TcpClientConfig client_config(uint16_t port) {
    TcpClientConfig c;
    c.server_port = port;
    c.backoff_initial_sec = 0.05;
    c.backoff_max_sec = 0.2;
    return c;
}

}  // namespace

TEST(EndToEnd, ClientDeliversSnapshotsToServer) {
    Received got;
    Server server(ServerConfig{0}, [&](const MetricSnapshot& s) { got.add(s); });
    server.start();

    TcpClient client(client_config(server.port()), "node-1");
    client.send_snapshot(snap("node-1", 11.0));
    client.send_snapshot(snap("node-1", 22.0));
    client.send_heartbeat();

    ASSERT_TRUE(wait_until([&] { return got.size() == 2; }));
    EXPECT_DOUBLE_EQ(got.items[0].cpu_percent, 11.0);  // order preserved
    EXPECT_DOUBLE_EQ(got.items[1].cpu_percent, 22.0);

    ASSERT_TRUE(wait_until([&] {
        auto n = server.registry().get("node-1");
        return n && n->messages == 4;  // hello + 2 metrics + heartbeat
    }));
    EXPECT_TRUE(server.registry().get("node-1")->up);
}

TEST(EndToEnd, ManyAgentsAtOnce) {
    Received got;
    Server server(ServerConfig{0}, [&](const MetricSnapshot& s) { got.add(s); });
    server.start();

    constexpr int kAgents = 8, kEach = 25;
    std::vector<std::thread> agents;
    for (int a = 0; a < kAgents; ++a) {
        agents.emplace_back([&, a] {
            std::string host = "node-" + std::to_string(a);
            TcpClient client(client_config(server.port()), host);
            for (int i = 0; i < kEach; ++i) client.send_snapshot(snap(host, i));
        });
    }
    for (auto& t : agents) t.join();

    ASSERT_TRUE(wait_until([&] { return got.size() == kAgents * kEach; }));
    EXPECT_EQ(server.registry().nodes().size(), static_cast<std::size_t>(kAgents));
}

TEST(EndToEnd, ClientReconnectsWhenServerStartsLater) {
    // Reserve a free port, then free it so the server is "down".
    uint16_t port;
    {
        net::Socket probe = net::listen_tcp(0);
        port = net::local_port(probe.fd());
    }

    TcpClient client(client_config(port), "node-late");
    std::atomic<bool> delivered{false};
    std::thread sender([&] {
        client.send_snapshot(snap("node-late", 50.0));  // blocks, retrying with backoff
        delivered = true;
    });

    std::this_thread::sleep_for(300ms);  // several failed attempts happen here
    EXPECT_FALSE(delivered);

    Received got;
    Server server(ServerConfig{port}, [&](const MetricSnapshot& s) { got.add(s); });
    server.start();

    ASSERT_TRUE(wait_until([&] { return delivered.load(); }));
    sender.join();
    ASSERT_TRUE(wait_until([&] { return got.size() == 1; }));
    EXPECT_DOUBLE_EQ(got.items[0].cpu_percent, 50.0);  // the snapshot was not lost
}

TEST(EndToEnd, ClientSurvivesServerRestartWithoutLosingData) {
    Received got;
    auto server = std::make_unique<Server>(ServerConfig{0}, [&](const MetricSnapshot& s) { got.add(s); });
    server->start();
    uint16_t port = server->port();

    TcpClient client(client_config(port), "node-r");
    client.send_snapshot(snap("node-r", 1.0));
    ASSERT_TRUE(wait_until([&] { return got.size() == 1; }));

    server->stop();  // server shuts down: its side of the connection sends FIN
    server = std::make_unique<Server>(ServerConfig{port}, [&](const MetricSnapshot& s) { got.add(s); });
    server->start();  // and comes back

    // The client must notice the old connection is closed BEFORE writing,
    // reconnect, and deliver every snapshot.
    std::this_thread::sleep_for(200ms);  // let the FIN arrive (as with a real 1 s interval)
    for (int i = 0; i < 3; ++i) client.send_snapshot(snap("node-r", 2.0 + i));
    ASSERT_TRUE(wait_until([&] { return got.size() == 4; }));
    EXPECT_DOUBLE_EQ(got.items[1].cpu_percent, 2.0);
    EXPECT_EQ(client.connects(), 2u);
}

TEST(EndToEnd, RequestStopAbortsReconnectQuickly) {
    uint16_t port;
    {
        net::Socket probe = net::listen_tcp(0);
        port = net::local_port(probe.fd());
    }
    TcpClientConfig cfg = client_config(port);
    cfg.backoff_initial_sec = 5.0;  // long waits between attempts
    TcpClient client(cfg, "node-x");

    std::atomic<bool> threw{false};
    std::thread sender([&] {
        try {
            client.send_snapshot(snap("node-x", 1.0));
        } catch (const std::exception&) {
            threw = true;
        }
    });
    std::this_thread::sleep_for(100ms);
    auto start = std::chrono::steady_clock::now();
    client.request_stop();
    sender.join();
    EXPECT_TRUE(threw);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);  // did not wait 5 s
}

TEST(EndToEnd, SilentNodeIsMarkedDown) {
    ServerConfig cfg{0, 300ms, 50ms};  // short timeout for the test
    std::mutex m;
    std::vector<std::string> events;
    Server server(cfg, nullptr, [&](const std::string& e) {
        std::lock_guard<std::mutex> lock(m);
        events.push_back(e);
    });
    server.start();

    {
        TcpClient client(client_config(server.port()), "node-quiet");
        client.send_heartbeat();
    }  // client goes away and sends nothing more

    ASSERT_TRUE(wait_until([&] {
        auto n = server.registry().get("node-quiet");
        return n && !n->up;
    }));
    std::lock_guard<std::mutex> lock(m);
    bool saw_down = false;
    for (const auto& e : events) saw_down |= e.find("node-quiet is DOWN") != std::string::npos;
    EXPECT_TRUE(saw_down);
}

TEST(EndToEnd, GarbageFromClientDoesNotCrashServer) {
    Received got;
    Server server(ServerConfig{0}, [&](const MetricSnapshot& s) { got.add(s); });
    server.start();

    {
        net::Socket bad = net::connect_tcp("127.0.0.1", server.port());
        net::send_frame(bad.fd(), "this is not json");  // server drops this connection
    }
    {
        net::Socket rude = net::connect_tcp("127.0.0.1", server.port());
        // Metrics without a hello first is also a protocol violation.
        net::send_frame(rude.fd(), protocol::encode_metrics(snap("x", 1)));
    }
    ASSERT_TRUE(wait_until([&] { return server.protocol_errors() >= 2; }));

    // A well-behaved client still works afterwards.
    TcpClient good(client_config(server.port()), "node-ok");
    good.send_snapshot(snap("node-ok", 5.0));
    ASSERT_TRUE(wait_until([&] { return got.size() == 1; }));
}
