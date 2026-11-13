// sysmon_server — receives metrics from agents, tracks which nodes are alive.
//
// Usage: sysmon_server [--port 9000] [--timeout SECONDS]
//   --timeout   mark a node DOWN after this many seconds without any message (default 15)

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "sysmon/server.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::mutex g_print_mutex;  // several handler threads print at once

std::string clock_time() {
    std::time_t t = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    sysmon::ServerConfig config;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
            config.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--timeout") && i + 1 < argc) {
            config.node_timeout = std::chrono::milliseconds(
                static_cast<long>(std::stod(argv[++i]) * 1000));
        } else {
            std::cerr << "Usage: " << argv[0] << " [--port PORT] [--timeout SECONDS]\n";
            return 1;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    auto on_metrics = [](const sysmon::MetricSnapshot& s) {
        std::lock_guard<std::mutex> lock(g_print_mutex);
        std::printf("%s  %-16s cpu %5.1f%%  mem %5.1f%%  disk r/w %7.1f/%7.1f kB/s  net rx/tx %7.1f/%7.1f kB/s  top: %s\n",
                    clock_time().c_str(), s.host.c_str(), s.cpu_percent, s.mem_used_percent,
                    s.disk_read_kbps, s.disk_write_kbps, s.net_rx_kbps, s.net_tx_kbps,
                    s.top_cpu.empty() ? "-" : s.top_cpu[0].name.c_str());
        std::fflush(stdout);
    };
    auto on_event = [](const std::string& msg) {
        std::lock_guard<std::mutex> lock(g_print_mutex);
        std::printf("%s  ** %s\n", clock_time().c_str(), msg.c_str());
        std::fflush(stdout);
    };

    try {
        sysmon::Server server(config, on_metrics, on_event);
        server.start();
        while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server.stop();
    } catch (const std::exception& e) {
        std::cerr << "sysmon_server: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
