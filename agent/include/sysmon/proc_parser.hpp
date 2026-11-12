#pragma once
// Parsers for Linux /proc files.
//
// Design: every parser takes a std::istream (or a string), not a file path.
// That keeps them pure and lets the unit tests feed in fake /proc content.
// ProcReader is the thin layer that opens the real files.

#include <istream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "sysmon/metrics.hpp"

namespace sysmon {

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---- Pure parsers ----
CpuTimes parse_cpu_times(std::istream& in);   // /proc/stat
MemInfo parse_meminfo(std::istream& in);      // /proc/meminfo
DiskStats parse_diskstats(std::istream& in);  // /proc/diskstats
NetStats parse_net_dev(std::istream& in);     // /proc/net/dev
LoadAvg parse_loadavg(std::istream& in);      // /proc/loadavg

// Parses one /proc/[pid]/stat line. Returns nullopt if the line is malformed
// (processes can exit while we read them, so this is not an exception).
std::optional<ProcessSample> parse_pid_stat(const std::string& line,
                                            uint64_t page_size_kb);

// ---- Helpers ----

// CPU busy % between two /proc/stat readings.
double cpu_percent(const CpuTimes& prev, const CpuTimes& cur);

// True if `name` is a whole disk, not a partition or virtual device.
// "sda" -> true, "sda1" -> false (prefix "sda" exists), "loop0" -> false.
bool is_physical_disk(const std::string& name,
                      const std::vector<std::string>& all_names);

// ---- File access ----
class ProcReader {
public:
    // `root` is "/proc" normally; tests point it at a fake directory.
    explicit ProcReader(std::string root = "/proc");

    CpuTimes cpu() const;
    MemInfo memory() const;
    DiskStats disk() const;
    NetStats network() const;
    LoadAvg load() const;
    std::vector<ProcessSample> processes() const;

    const std::string& root() const { return root_; }

private:
    std::string root_;
    uint64_t page_size_kb_;
};

}  // namespace sysmon
