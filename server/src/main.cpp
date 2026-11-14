// sysmon_server — receives metrics from agents, tracks which nodes are alive,
// and (with --db) stores everything in PostgreSQL.
//
// Usage: sysmon_server [--port 9000] [--timeout SECONDS] [--db CONNINFO] [--quiet]
//   --timeout   mark a node DOWN after this many seconds without any message (default 15)
//   --db        PostgreSQL connection string, e.g.
//               postgresql://sysmon:sysmon@localhost:5432/sysmon
//   --quiet     don't print every snapshot (only events and periodic DB stats)

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "sysmon/db_writer.hpp"
#include "sysmon/pg_store.hpp"
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

void log_line(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::printf("%s  ** %s\n", clock_time().c_str(), msg.c_str());
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    sysmon::ServerConfig config;
    std::string db_conninfo;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
            config.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        } else if (!std::strcmp(argv[i], "--timeout") && i + 1 < argc) {
            config.node_timeout = std::chrono::milliseconds(
                static_cast<long>(std::stod(argv[++i]) * 1000));
        } else if (!std::strcmp(argv[i], "--db") && i + 1 < argc) {
            db_conninfo = argv[++i];
        } else if (!std::strcmp(argv[i], "--quiet")) {
            quiet = true;
        } else {
            std::cerr << "Usage: " << argv[0]
                      << " [--port PORT] [--timeout SECONDS] [--db CONNINFO] [--quiet]\n";
            return 1;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    try {
        // ---- optional database ----
        std::unique_ptr<sysmon::DbWriter> writer;
        if (!db_conninfo.empty()) {
            auto store = std::make_shared<sysmon::PgStore>(db_conninfo);
            try {
                store->ensure_ready();  // connect + create tables
                log_line("database connected, schema ready");
            } catch (const std::exception& e) {
                // Not fatal: the writer keeps retrying, data is buffered meanwhile.
                log_line(std::string("database not reachable yet (will retry): ") + e.what());
            }
            writer = std::make_unique<sysmon::DbWriter>(store);
            writer->start();
        }

        auto on_metrics = [&](const sysmon::MetricSnapshot& s) {
            if (writer) writer->submit(s);  // just enqueues: never waits for the DB
            if (quiet) return;
            std::lock_guard<std::mutex> lock(g_print_mutex);
            std::printf("%s  %-12s cpu %5.1f%%  mem %5.1f%%  disk r/w %6.1f/%6.1f kB/s  net rx/tx %6.1f/%6.1f kB/s\n",
                        clock_time().c_str(), s.host.c_str(), s.cpu_percent, s.mem_used_percent,
                        s.disk_read_kbps, s.disk_write_kbps, s.net_rx_kbps, s.net_tx_kbps);
            std::fflush(stdout);
        };

        sysmon::Server server(config, on_metrics, log_line);
        if (writer) {
            server.set_status_handler([&](const std::string& host, const std::string& addr,
                                          bool up, std::int64_t ts) {
                writer->submit(sysmon::NodeEvent{host, addr, up, ts});
            });
        }
        server.start();

        // Main thread: wait for Ctrl+C; print DB stats every 10 s.
        auto last_stats = std::chrono::steady_clock::now();
        while (!g_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (writer && std::chrono::steady_clock::now() - last_stats > std::chrono::seconds(10)) {
                last_stats = std::chrono::steady_clock::now();
                log_line("db: written=" + std::to_string(writer->written()) +
                         " batches=" + std::to_string(writer->batches()) +
                         " pending=" + std::to_string(writer->pending()) +
                         " failures=" + std::to_string(writer->failures()) +
                         " dropped=" + std::to_string(writer->dropped()));
            }
        }

        server.stop();             // 1. no more incoming data
        if (writer) writer->stop();  // 2. flush what is queued to the database
        if (writer) {
            log_line("db final: written=" + std::to_string(writer->written()) +
                     " lost=" + std::to_string(writer->lost_at_shutdown() + writer->dropped()));
        }
    } catch (const std::exception& e) {
        std::cerr << "sysmon_server: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
