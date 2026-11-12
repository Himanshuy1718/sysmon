// sysmon_agent — Milestone 1: sample /proc and print metrics to the terminal.
// Later milestones replace the printing with a sender thread + TCP.
//
// Usage: sysmon_agent [--interval SECONDS] [--count N]   (count 0 = run forever)

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "sysmon/collector.hpp"

namespace {

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

}  // namespace

int main(int argc, char** argv) {
    double interval = 2.0;
    long count = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--interval") && i + 1 < argc) {
            interval = std::stod(argv[++i]);
        } else if (!std::strcmp(argv[i], "--count") && i + 1 < argc) {
            count = std::stol(argv[++i]);
        } else {
            std::cerr << "Usage: " << argv[0] << " [--interval SECONDS] [--count N]\n";
            return 1;
        }
    }
    if (interval <= 0) interval = 2.0;

    try {
        sysmon::Collector collector;  // takes the first (baseline) reading
        for (long n = 0; count == 0 || n < count; ++n) {
            std::this_thread::sleep_for(std::chrono::duration<double>(interval));
            print_snapshot(collector.sample());
        }
    } catch (const std::exception& e) {
        std::cerr << "sysmon_agent: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
