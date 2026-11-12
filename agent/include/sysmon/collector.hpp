#pragma once
// Collector turns cumulative /proc counters into rates.
//
// Most /proc values are counters since boot (CPU ticks, bytes sent, sectors
// written). A single reading says nothing about "now"; we need two readings
// and the time between them:  rate = (cur - prev) / elapsed.
// So the Collector remembers the previous reading.

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>

#include "sysmon/metrics.hpp"
#include "sysmon/proc_parser.hpp"

namespace sysmon {

class Collector {
public:
    explicit Collector(ProcReader reader = ProcReader{}, std::size_t top_n = 5);

    // Takes a new reading and returns metrics for the interval since the
    // previous call (or since construction for the first call).
    MetricSnapshot sample();

    // Same as sample() but with an explicit elapsed time — used by tests so
    // results do not depend on the real clock.
    MetricSnapshot sample_with_elapsed(double elapsed_sec);

private:
    struct Raw {
        CpuTimes cpu;
        DiskStats disk;
        NetStats net;
        std::unordered_map<int, ProcessSample> procs;
    };
    Raw read_raw() const;

    ProcReader reader_;
    std::size_t top_n_;
    std::string host_;
    long clk_tck_;
    Raw prev_;
    std::chrono::steady_clock::time_point prev_time_;
};

// Rate in kB/s from two cumulative byte counters.
double kb_per_sec(uint64_t prev_bytes, uint64_t cur_bytes, double elapsed_sec);

}  // namespace sysmon
