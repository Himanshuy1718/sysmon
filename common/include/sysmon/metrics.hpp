#pragma once
// Plain data types for raw /proc readings and the final computed snapshot.

#include <cstdint>
#include <string>
#include <vector>

namespace sysmon {

// ---- Raw counters read from /proc (cumulative since boot) ----

// First line of /proc/stat, in clock ticks (jiffies).
struct CpuTimes {
    uint64_t user = 0, nice = 0, system = 0, idle = 0;
    uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;

    uint64_t idle_total() const { return idle + iowait; }
    uint64_t total() const {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }
};

// /proc/meminfo, values in kB.
struct MemInfo {
    uint64_t total_kb = 0;
    uint64_t available_kb = 0;
    uint64_t swap_total_kb = 0;
    uint64_t swap_free_kb = 0;
};

// /proc/diskstats summed over physical disks. 1 sector = 512 bytes.
struct DiskStats {
    uint64_t sectors_read = 0;
    uint64_t sectors_written = 0;
};

// /proc/net/dev summed over all interfaces except loopback.
struct NetStats {
    uint64_t rx_bytes = 0;
    uint64_t tx_bytes = 0;
};

// /proc/loadavg
struct LoadAvg {
    double load1 = 0, load5 = 0, load15 = 0;
};

// One line of /proc/[pid]/stat.
struct ProcessSample {
    int pid = 0;
    std::string name;
    uint64_t cpu_ticks = 0;  // utime + stime
    uint64_t rss_kb = 0;
};

// ---- Computed values (rates / percentages between two samples) ----

struct ProcessUsage {
    int pid = 0;
    std::string name;
    double cpu_percent = 0;  // like `top`: 100% = one full core
    uint64_t rss_kb = 0;
};

struct MetricSnapshot {
    std::string host;
    int64_t timestamp_ms = 0;  // Unix epoch milliseconds

    double cpu_percent = 0;
    double mem_used_percent = 0;
    uint64_t mem_used_kb = 0;
    uint64_t mem_total_kb = 0;

    double disk_read_kbps = 0;
    double disk_write_kbps = 0;
    double net_rx_kbps = 0;
    double net_tx_kbps = 0;

    LoadAvg load;
    std::vector<ProcessUsage> top_cpu;  // highest CPU first
    std::vector<ProcessUsage> top_mem;  // highest RSS first
};

}  // namespace sysmon
