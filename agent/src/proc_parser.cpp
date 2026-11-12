#include "sysmon/proc_parser.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace sysmon {

// ---------------------------------------------------------------------------
// /proc/stat — first line:
//   cpu  user nice system idle iowait irq softirq steal guest guest_nice
// All values are cumulative clock ticks since boot.
// ---------------------------------------------------------------------------
CpuTimes parse_cpu_times(std::istream& in) {
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string label;
        ss >> label;
        if (label != "cpu") continue;  // skip cpu0, cpu1, intr, ...

        CpuTimes t;
        ss >> t.user >> t.nice >> t.system >> t.idle;
        if (!ss) throw ParseError("/proc/stat: cpu line too short");
        // Older kernels may not have the remaining fields; leave them 0.
        ss >> t.iowait >> t.irq >> t.softirq >> t.steal;
        return t;
    }
    throw ParseError("/proc/stat: no aggregate 'cpu' line");
}

// ---------------------------------------------------------------------------
// /proc/meminfo — "Key:   value kB" per line.
// ---------------------------------------------------------------------------
MemInfo parse_meminfo(std::istream& in) {
    std::unordered_map<std::string, uint64_t> kv;
    std::string line;
    while (std::getline(in, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::istringstream ss(line.substr(colon + 1));
        uint64_t value = 0;
        if (ss >> value) kv[line.substr(0, colon)] = value;
    }

    if (!kv.count("MemTotal")) throw ParseError("/proc/meminfo: no MemTotal");

    MemInfo m;
    m.total_kb = kv["MemTotal"];
    // MemAvailable exists since Linux 3.14; fall back to MemFree on old kernels.
    m.available_kb = kv.count("MemAvailable") ? kv["MemAvailable"] : kv["MemFree"];
    m.swap_total_kb = kv["SwapTotal"];
    m.swap_free_kb = kv["SwapFree"];
    return m;
}

// ---------------------------------------------------------------------------
// /proc/diskstats — per line:
//   major minor name reads merged sectors_read ms_read writes merged sectors_written ...
// Partitions (sda1) are also listed, so we only count whole disks to avoid
// counting the same I/O twice.
// ---------------------------------------------------------------------------
bool is_physical_disk(const std::string& name,
                      const std::vector<std::string>& all_names) {
    static const char* kVirtual[] = {"loop", "ram", "zram", "dm-", "md", "sr"};
    for (const char* prefix : kVirtual) {
        if (name.rfind(prefix, 0) == 0) return false;
    }
    // A partition's name starts with its parent's name: sda1 / sda, nvme0n1p1 / nvme0n1.
    for (const auto& other : all_names) {
        if (other != name && name.size() > other.size() &&
            name.rfind(other, 0) == 0) {
            return false;
        }
    }
    return true;
}

DiskStats parse_diskstats(std::istream& in) {
    struct Row {
        std::string name;
        uint64_t sectors_read, sectors_written;
    };
    std::vector<Row> rows;
    std::vector<std::string> names;

    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        unsigned major, minor;
        Row r;
        uint64_t reads, reads_merged, ms_read, writes, writes_merged;
        ss >> major >> minor >> r.name >> reads >> reads_merged >> r.sectors_read >>
            ms_read >> writes >> writes_merged >> r.sectors_written;
        if (!ss) continue;  // skip malformed lines
        names.push_back(r.name);
        rows.push_back(r);
    }

    DiskStats d;
    for (const auto& r : rows) {
        if (!is_physical_disk(r.name, names)) continue;
        d.sectors_read += r.sectors_read;
        d.sectors_written += r.sectors_written;
    }
    return d;
}

// ---------------------------------------------------------------------------
// /proc/net/dev — two header lines, then:
//   iface: rx_bytes rx_packets rx_errs rx_drop rx_fifo rx_frame rx_compressed
//          rx_multicast tx_bytes tx_packets ...
// ---------------------------------------------------------------------------
NetStats parse_net_dev(std::istream& in) {
    NetStats n;
    std::string line;
    while (std::getline(in, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;  // header lines have no ':' after iface

        std::string iface = line.substr(0, colon);
        iface.erase(0, iface.find_first_not_of(' '));
        if (iface == "lo") continue;  // loopback traffic is not real network I/O

        std::istringstream ss(line.substr(colon + 1));
        uint64_t f[9] = {};
        for (auto& v : f) ss >> v;
        if (!ss) continue;
        n.rx_bytes += f[0];
        n.tx_bytes += f[8];
    }
    return n;
}

// ---------------------------------------------------------------------------
// /proc/loadavg — "0.52 0.58 0.59 1/389 12345"
// ---------------------------------------------------------------------------
LoadAvg parse_loadavg(std::istream& in) {
    LoadAvg l;
    in >> l.load1 >> l.load5 >> l.load15;
    if (!in) throw ParseError("/proc/loadavg: malformed");
    return l;
}

// ---------------------------------------------------------------------------
// /proc/[pid]/stat — "pid (comm) state ppid ... utime stime ... rss ..."
// The command name can contain spaces and ')' itself, e.g. "(Web Content)",
// so we take everything between the FIRST '(' and the LAST ')'.
// Fields after ')' (0-based): 0=state ... 11=utime 12=stime ... 21=rss (pages)
// ---------------------------------------------------------------------------
std::optional<ProcessSample> parse_pid_stat(const std::string& line,
                                            uint64_t page_size_kb) {
    auto open = line.find('(');
    auto close = line.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        return std::nullopt;
    }

    ProcessSample p;
    try {
        p.pid = std::stoi(line.substr(0, open));
    } catch (...) {
        return std::nullopt;
    }
    p.name = line.substr(open + 1, close - open - 1);

    std::istringstream ss(line.substr(close + 1));
    std::vector<std::string> fields;
    std::string tok;
    while (ss >> tok) fields.push_back(tok);
    if (fields.size() < 22) return std::nullopt;

    try {
        uint64_t utime = std::stoull(fields[11]);
        uint64_t stime = std::stoull(fields[12]);
        long long rss_pages = std::stoll(fields[21]);
        p.cpu_ticks = utime + stime;
        p.rss_kb = rss_pages > 0 ? static_cast<uint64_t>(rss_pages) * page_size_kb : 0;
    } catch (...) {
        return std::nullopt;
    }
    return p;
}

// ---------------------------------------------------------------------------
// CPU % = share of non-idle ticks in the interval between two readings.
// ---------------------------------------------------------------------------
double cpu_percent(const CpuTimes& prev, const CpuTimes& cur) {
    if (cur.total() <= prev.total()) return 0.0;
    double total = static_cast<double>(cur.total() - prev.total());
    double idle = static_cast<double>(cur.idle_total() - prev.idle_total());
    double pct = (total - idle) / total * 100.0;
    return std::clamp(pct, 0.0, 100.0);
}

// ---------------------------------------------------------------------------
// ProcReader
// ---------------------------------------------------------------------------
namespace {
std::ifstream open_or_throw(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw ParseError("cannot open " + path);
    return f;
}
}  // namespace

ProcReader::ProcReader(std::string root) : root_(std::move(root)) {
    long page = sysconf(_SC_PAGESIZE);
    page_size_kb_ = page > 0 ? static_cast<uint64_t>(page) / 1024 : 4;
}

CpuTimes ProcReader::cpu() const {
    auto f = open_or_throw(root_ + "/stat");
    return parse_cpu_times(f);
}

MemInfo ProcReader::memory() const {
    auto f = open_or_throw(root_ + "/meminfo");
    return parse_meminfo(f);
}

DiskStats ProcReader::disk() const {
    std::ifstream f(root_ + "/diskstats");
    return f ? parse_diskstats(f) : DiskStats{};  // missing in some containers
}

NetStats ProcReader::network() const {
    std::ifstream f(root_ + "/net/dev");
    return f ? parse_net_dev(f) : NetStats{};
}

LoadAvg ProcReader::load() const {
    auto f = open_or_throw(root_ + "/loadavg");
    return parse_loadavg(f);
}

std::vector<ProcessSample> ProcReader::processes() const {
    std::vector<ProcessSample> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root_, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() ||
            !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); })) {
            continue;  // only numeric directories are processes
        }
        std::ifstream f(entry.path() / "stat");
        std::string line;
        if (!f || !std::getline(f, line)) continue;  // process exited meanwhile
        if (auto p = parse_pid_stat(line, page_size_kb_)) out.push_back(std::move(*p));
    }
    return out;
}

}  // namespace sysmon
