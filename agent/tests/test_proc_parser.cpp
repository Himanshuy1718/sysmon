#include <gtest/gtest.h>

#include <sstream>

#include "sysmon/proc_parser.hpp"

using namespace sysmon;

// ---------------- /proc/stat ----------------

TEST(ParseCpuTimes, ReadsAggregateLineOnly) {
    std::istringstream in(
        "cpu  100 5 50 800 20 3 2 1 0 0\n"
        "cpu0 50 2 25 400 10 1 1 0 0 0\n"
        "intr 12345\n");
    CpuTimes t = parse_cpu_times(in);
    EXPECT_EQ(t.user, 100u);
    EXPECT_EQ(t.nice, 5u);
    EXPECT_EQ(t.system, 50u);
    EXPECT_EQ(t.idle, 800u);
    EXPECT_EQ(t.iowait, 20u);
    EXPECT_EQ(t.steal, 1u);
    EXPECT_EQ(t.total(), 981u);
    EXPECT_EQ(t.idle_total(), 820u);
}

TEST(ParseCpuTimes, ThrowsWhenMissing) {
    std::istringstream in("intr 1 2 3\n");
    EXPECT_THROW(parse_cpu_times(in), ParseError);
}

TEST(CpuPercent, ComputesBusyShareOfInterval) {
    CpuTimes a{100, 0, 100, 800, 0, 0, 0, 0};  // total 1000
    CpuTimes b{150, 0, 150, 900, 0, 0, 0, 0};  // +200 total, +100 idle
    EXPECT_DOUBLE_EQ(cpu_percent(a, b), 50.0);
}

TEST(CpuPercent, IowaitCountsAsIdle) {
    CpuTimes a{0, 0, 0, 0, 0, 0, 0, 0};
    CpuTimes b{25, 0, 0, 50, 25, 0, 0, 0};  // 75 of 100 idle (idle + iowait)
    EXPECT_DOUBLE_EQ(cpu_percent(a, b), 25.0);
}

TEST(CpuPercent, ZeroWhenNoTimePassed) {
    CpuTimes a{1, 1, 1, 1, 0, 0, 0, 0};
    EXPECT_DOUBLE_EQ(cpu_percent(a, a), 0.0);
}

// ---------------- /proc/meminfo ----------------

TEST(ParseMeminfo, ReadsKeyFields) {
    std::istringstream in(
        "MemTotal:       16000000 kB\n"
        "MemFree:         2000000 kB\n"
        "MemAvailable:    8000000 kB\n"
        "Buffers:          100000 kB\n"
        "SwapTotal:       4000000 kB\n"
        "SwapFree:        3000000 kB\n");
    MemInfo m = parse_meminfo(in);
    EXPECT_EQ(m.total_kb, 16000000u);
    EXPECT_EQ(m.available_kb, 8000000u);
    EXPECT_EQ(m.swap_total_kb, 4000000u);
    EXPECT_EQ(m.swap_free_kb, 3000000u);
}

TEST(ParseMeminfo, FallsBackToMemFreeOnOldKernels) {
    std::istringstream in("MemTotal: 1000 kB\nMemFree: 300 kB\n");
    EXPECT_EQ(parse_meminfo(in).available_kb, 300u);
}

TEST(ParseMeminfo, ThrowsWithoutMemTotal) {
    std::istringstream in("MemFree: 300 kB\n");
    EXPECT_THROW(parse_meminfo(in), ParseError);
}

// ---------------- /proc/diskstats ----------------

TEST(IsPhysicalDisk, FiltersPartitionsAndVirtualDevices) {
    std::vector<std::string> all = {"loop0", "sda", "sda1", "sda2", "sdb",
                                    "nvme0n1", "nvme0n1p1", "dm-0", "ram0"};
    EXPECT_TRUE(is_physical_disk("sda", all));
    EXPECT_TRUE(is_physical_disk("sdb", all));
    EXPECT_TRUE(is_physical_disk("nvme0n1", all));
    EXPECT_FALSE(is_physical_disk("sda1", all));
    EXPECT_FALSE(is_physical_disk("nvme0n1p1", all));
    EXPECT_FALSE(is_physical_disk("loop0", all));
    EXPECT_FALSE(is_physical_disk("dm-0", all));
    EXPECT_FALSE(is_physical_disk("ram0", all));
}

TEST(ParseDiskstats, SumsOnlyWholeDisks) {
    std::istringstream in(
        "   7       0 loop0 10 0 999 0 0 0 999 0 0 0 0\n"
        "   8       0 sda 100 0 2000 50 40 0 1000 30 0 60 80\n"
        "   8       1 sda1 90 0 1800 45 35 0 900 25 0 55 70\n"
        "   8      16 sdb 10 0 500 5 4 0 300 3 0 6 8\n");
    DiskStats d = parse_diskstats(in);
    EXPECT_EQ(d.sectors_read, 2500u);     // sda + sdb, not sda1, not loop0
    EXPECT_EQ(d.sectors_written, 1300u);
}

// ---------------- /proc/net/dev ----------------

TEST(ParseNetDev, SkipsHeadersAndLoopback) {
    std::istringstream in(
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
        "    lo:  5000      50    0    0    0     0          0         0     5000      50    0    0    0     0       0          0\n"
        "  eth0: 10000     100    0    0    0     0          0         0     4000      40    0    0    0     0       0          0\n"
        " wlan0:  2000      20    0    0    0     0          0         0     1000      10    0    0    0     0       0          0\n");
    NetStats n = parse_net_dev(in);
    EXPECT_EQ(n.rx_bytes, 12000u);
    EXPECT_EQ(n.tx_bytes, 5000u);
}

// ---------------- /proc/loadavg ----------------

TEST(ParseLoadavg, ReadsThreeAverages) {
    std::istringstream in("0.52 0.58 1.25 1/389 12345\n");
    LoadAvg l = parse_loadavg(in);
    EXPECT_DOUBLE_EQ(l.load1, 0.52);
    EXPECT_DOUBLE_EQ(l.load5, 0.58);
    EXPECT_DOUBLE_EQ(l.load15, 1.25);
}

// ---------------- /proc/[pid]/stat ----------------

namespace {
// Builds a realistic stat line: utime=field 14, stime=15, rss=24 (1-based).
std::string stat_line(const std::string& pid_and_comm, int utime, int stime, int rss_pages) {
    std::ostringstream ss;
    ss << pid_and_comm << " S 1 1 1 0 -1 4194560 100 0 0 0 " << utime << ' ' << stime
       << " 0 0 20 0 1 0 12345 100000000 " << rss_pages << " 18446744073709551615";
    return ss.str();
}
}  // namespace

TEST(ParsePidStat, ReadsTicksAndRss) {
    auto p = parse_pid_stat(stat_line("42 (nginx)", 300, 200, 1000), 4);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->pid, 42);
    EXPECT_EQ(p->name, "nginx");
    EXPECT_EQ(p->cpu_ticks, 500u);
    EXPECT_EQ(p->rss_kb, 4000u);
}

TEST(ParsePidStat, HandlesSpacesAndParensInName) {
    auto p = parse_pid_stat(stat_line("7 (Web Content)", 1, 1, 1), 4);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->name, "Web Content");

    auto q = parse_pid_stat(stat_line("8 (weird) name)", 1, 1, 1), 4);
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->name, "weird) name");
    EXPECT_EQ(q->cpu_ticks, 2u);
}

TEST(ParsePidStat, RejectsMalformedLines) {
    EXPECT_FALSE(parse_pid_stat("", 4).has_value());
    EXPECT_FALSE(parse_pid_stat("12 (short) S 1 2", 4).has_value());
    EXPECT_FALSE(parse_pid_stat("abc (x) S", 4).has_value());
}
