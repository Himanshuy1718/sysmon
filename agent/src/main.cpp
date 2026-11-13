// sysmon_agent — collects metrics and sends them to the server
// (or prints them, when no --server is given).
//
// Usage:
//   sysmon_agent [--server HOST:PORT] [--interval SECONDS] [--heartbeat SECONDS]
//                [--count N] [--queue N] [--sink-delay MS]
//
//   --server      send to this server over TCP (e.g. 127.0.0.1:9000)
//   --heartbeat   with --server: "I'm alive" message if idle this long (default 5)
//   --name        node name reported to the server (default: this machine's hostname);
//                 lets you run several agents on one laptop for testing
//   --interval    seconds between samples (default 2)
//   --count       stop after N snapshots are sent (default 0 = run until Ctrl+C)
//   --queue       queue capacity before the oldest snapshot is dropped (default 100)
//   --sink-delay  make the sender artificially slow, to see the queue absorb it
//                 and drop old data (demo of why the threads are decoupled)

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "sysmon/agent.hpp"
#include "sysmon/tcp_client.hpp"

namespace {

// Set by the Ctrl+C handler. A signal handler may only touch lock-free
// atomics / sig_atomic_t — never locks, printf or memory allocation.
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

void print_snapshot(const sysmon::MetricSnapshot& s) {
    std::printf("\n[%s]  t=%lld\n", s.host.c_str(), static_cast<long long>(s.timestamp_ms));
    std::printf("  CPU   %5.1f %%      load %.2f %.2f %.2f\n", s.cpu_percent, s.load.load1,
                s.load.load5, s.load.load15);
    std::printf("  MEM   %5.1f %%      %llu / %llu MB\n", s.mem_used_percent,
                static_cast<unsigned long long>(s.mem_used_kb / 1024),
                static_cast<unsigned long long>(s.mem_total_kb / 1024));
    std::printf("  DISK  read %8.1f kB/s   write %8.1f kB/s\n", s.disk_read_kbps, s.disk_write_kbps);
    std::printf("  NET   rx   %8.1f kB/s   tx    %8.1f kB/s\n", s.net_rx_kbps, s.net_tx_kbps);

    std::printf("  Top CPU:");
    for (const auto& p : s.top_cpu) std::printf("  %s(%d) %.1f%%", p.name.c_str(), p.pid, p.cpu_percent);
    std::printf("\n  Top MEM:");
    for (const auto& p : s.top_mem)
        std::printf("  %s(%d) %lluMB", p.name.c_str(), p.pid,
                    static_cast<unsigned long long>(p.rss_kb / 1024));
    std::printf("\n");
    std::fflush(stdout);
}

void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " [--server HOST:PORT] [--name NAME] [--interval SECONDS] [--heartbeat SECONDS]"
                 " [--count N] [--queue N] [--sink-delay MS]\n";
}

}  // namespace

int main(int argc, char** argv) {
    sysmon::AgentConfig config;
    long count = 0;
    long sink_delay_ms = 0;
    std::string server;  // "host:port", empty = print to terminal
    std::string name;    // overrides hostname
    config.heartbeat_sec = 5.0;

    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage(argv[0]);
                std::exit(1);
            }
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--interval")) config.interval_sec = std::stod(next());
        else if (!std::strcmp(argv[i], "--count")) count = std::stol(next());
        else if (!std::strcmp(argv[i], "--queue")) config.queue_capacity = std::stoul(next());
        else if (!std::strcmp(argv[i], "--sink-delay")) sink_delay_ms = std::stol(next());
        else if (!std::strcmp(argv[i], "--server")) server = next();
        else if (!std::strcmp(argv[i], "--name")) name = next();
        else if (!std::strcmp(argv[i], "--heartbeat")) config.heartbeat_sec = std::stod(next());
        else {
            usage(argv[0]);
            return 1;
        }
    }
    if (config.interval_sec <= 0) config.interval_sec = 2.0;

    std::signal(SIGINT, on_signal);   // Ctrl+C
    std::signal(SIGTERM, on_signal);  // `kill` / docker stop

    try {
        sysmon::Sink sink;
        sysmon::Heartbeat heartbeat;
        std::unique_ptr<sysmon::TcpClient> client;

        if (!server.empty()) {
            // ---- network mode ----
            auto colon = server.rfind(':');
            if (colon == std::string::npos) {
                usage(argv[0]);
                return 1;
            }
            sysmon::TcpClientConfig cc;
            cc.server_host = server.substr(0, colon);
            cc.server_port = static_cast<std::uint16_t>(std::stoi(server.substr(colon + 1)));

            if (name.empty()) {
                char host[256] = {};
                gethostname(host, sizeof(host) - 1);
                name = host;
            }
            client = std::make_unique<sysmon::TcpClient>(cc, name);
            sink = [&client, name](const sysmon::MetricSnapshot& s) {
                sysmon::MetricSnapshot named = s;
                named.host = name;  // snapshot carries the same node name as the hello
                client->send_snapshot(named);
            };
            heartbeat = [&client] { client->send_heartbeat(); };
        } else {
            // ---- print mode (as in Milestone 2) ----
            config.heartbeat_sec = 0;
            sink = [sink_delay_ms](const sysmon::MetricSnapshot& s) {
                if (sink_delay_ms > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(sink_delay_ms));
                print_snapshot(s);
            };
        }

        sysmon::Agent agent(sysmon::Collector{}, sink, config, heartbeat);
        // On shutdown, don't sit in a reconnect backoff if the server is down.
        if (client) agent.set_on_stopping([&client] { client->request_stop(); });
        agent.start();
        std::cerr << "sysmon_agent started (interval " << config.interval_sec << "s, "
                  << (server.empty() ? std::string("printing") : "server " + server)
                  << "). Ctrl+C to stop.\n";

        // The main thread just waits; the real work happens on the two agent threads.
        while (!g_stop && (count == 0 || static_cast<long>(agent.sent()) < count)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        agent.stop();  // graceful: stop collector, drain queue, join threads
        std::cerr << "\nstopped. collected=" << agent.collected() << " sent=" << agent.sent()
                  << " dropped=" << agent.dropped() << " heartbeats=" << agent.heartbeats()
                  << " errors=" << agent.errors() << "\n";
        if (agent.unsent_at_shutdown() > 0) {
            std::cerr << "note: " << agent.unsent_at_shutdown()
                      << " queued snapshot(s) were not delivered (server unreachable at shutdown)\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "sysmon_agent: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
