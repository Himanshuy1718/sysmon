#include "sysmon/collector.hpp"

#include <unistd.h>

#include <algorithm>

namespace sysmon {

namespace {
constexpr uint64_t kSectorBytes = 512;

std::string hostname() {
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) != 0) return "unknown";
    return buf;
}

int64_t now_epoch_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
}  // namespace

double kb_per_sec(uint64_t prev_bytes, uint64_t cur_bytes, double elapsed_sec) {
    // Counters can reset (interface re-created, wrap-around): report 0, not a huge number.
    if (elapsed_sec <= 0 || cur_bytes < prev_bytes) return 0.0;
    return static_cast<double>(cur_bytes - prev_bytes) / 1024.0 / elapsed_sec;
}

Collector::Collector(ProcReader reader, std::size_t top_n)
    : reader_(std::move(reader)),
      top_n_(top_n),
      host_(hostname()),
      clk_tck_(sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100),
      prev_(read_raw()),
      prev_time_(std::chrono::steady_clock::now()) {}

Collector::Raw Collector::read_raw() const {
    Raw r;
    r.cpu = reader_.cpu();
    r.disk = reader_.disk();
    r.net = reader_.network();
    for (auto& p : reader_.processes()) r.procs.emplace(p.pid, std::move(p));
    return r;
}

MetricSnapshot Collector::sample() {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - prev_time_).count();
    prev_time_ = now;
    return sample_with_elapsed(elapsed);
}

MetricSnapshot Collector::sample_with_elapsed(double elapsed_sec) {
    Raw cur = read_raw();
    MemInfo mem = reader_.memory();

    MetricSnapshot s;
    s.host = host_;
    s.timestamp_ms = now_epoch_ms();

    // CPU
    s.cpu_percent = cpu_percent(prev_.cpu, cur.cpu);

    // Memory (instantaneous, not a rate)
    s.mem_total_kb = mem.total_kb;
    s.mem_used_kb = mem.total_kb > mem.available_kb ? mem.total_kb - mem.available_kb : 0;
    s.mem_used_percent =
        mem.total_kb ? 100.0 * static_cast<double>(s.mem_used_kb) / static_cast<double>(mem.total_kb) : 0.0;

    // Disk and network rates
    s.disk_read_kbps = kb_per_sec(prev_.disk.sectors_read * kSectorBytes,
                                  cur.disk.sectors_read * kSectorBytes, elapsed_sec);
    s.disk_write_kbps = kb_per_sec(prev_.disk.sectors_written * kSectorBytes,
                                   cur.disk.sectors_written * kSectorBytes, elapsed_sec);
    s.net_rx_kbps = kb_per_sec(prev_.net.rx_bytes, cur.net.rx_bytes, elapsed_sec);
    s.net_tx_kbps = kb_per_sec(prev_.net.tx_bytes, cur.net.tx_bytes, elapsed_sec);

    s.load = reader_.load();

    // Per-process CPU% needs the same PID in both readings.
    std::vector<ProcessUsage> usage;
    usage.reserve(cur.procs.size());
    for (const auto& [pid, p] : cur.procs) {
        ProcessUsage u{pid, p.name, 0.0, p.rss_kb};
        auto it = prev_.procs.find(pid);
        if (it != prev_.procs.end() && p.cpu_ticks >= it->second.cpu_ticks && elapsed_sec > 0) {
            double ticks = static_cast<double>(p.cpu_ticks - it->second.cpu_ticks);
            u.cpu_percent = ticks / (elapsed_sec * static_cast<double>(clk_tck_)) * 100.0;
        }
        usage.push_back(std::move(u));
    }

    auto take_top = [&](auto cmp) {
        std::size_t n = std::min(top_n_, usage.size());
        std::partial_sort(usage.begin(), usage.begin() + static_cast<long>(n), usage.end(), cmp);
        return std::vector<ProcessUsage>(usage.begin(), usage.begin() + static_cast<long>(n));
    };
    s.top_cpu = take_top([](const ProcessUsage& a, const ProcessUsage& b) {
        return a.cpu_percent > b.cpu_percent;
    });
    s.top_mem = take_top([](const ProcessUsage& a, const ProcessUsage& b) {
        return a.rss_kb > b.rss_kb;
    });

    prev_ = std::move(cur);
    return s;
}

}  // namespace sysmon
