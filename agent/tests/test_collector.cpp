// Tests the Collector against a fake /proc directory, so rates and top-N
// lists can be checked with exact numbers.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>

#include "sysmon/collector.hpp"

namespace fs = std::filesystem;
using namespace sysmon;

namespace {

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << content;
}

std::string pid_stat(int pid, const std::string& name, int utime, int rss_pages) {
    return std::to_string(pid) + " (" + name + ") S 1 1 1 0 -1 0 0 0 0 0 " +
           std::to_string(utime) + " 0 0 0 20 0 1 0 1 1000 " + std::to_string(rss_pages) + " 0\n";
}

class FakeProc : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        std::random_device rd;
        root = fs::temp_directory_path() / ("fakeproc_" + std::to_string(rd()));
        write_state(/*cpu_busy=*/100, /*cpu_idle=*/900, /*disk_sectors=*/0,
                    /*rx=*/0, /*tx=*/0, /*ticks_a=*/0, /*ticks_b=*/0);
        write_file(root / "meminfo", "MemTotal: 1000000 kB\nMemAvailable: 250000 kB\n");
        write_file(root / "loadavg", "1.00 0.50 0.25 1/100 999\n");
    }
    void TearDown() override { fs::remove_all(root); }

    void write_state(int cpu_busy, int cpu_idle, int disk_sectors, int rx, int tx,
                     int ticks_a, int ticks_b) {
        write_file(root / "stat", "cpu  " + std::to_string(cpu_busy) + " 0 0 " +
                                      std::to_string(cpu_idle) + " 0 0 0 0\n");
        write_file(root / "diskstats",
                   "8 0 sda 0 0 " + std::to_string(disk_sectors) + " 0 0 0 " +
                       std::to_string(disk_sectors) + " 0 0 0 0\n");
        write_file(root / "net" / "dev",
                   "h1\nh2\n eth0: " + std::to_string(rx) + " 0 0 0 0 0 0 0 " +
                       std::to_string(tx) + " 0 0 0 0 0 0 0\n");
        write_file(root / "10" / "stat", pid_stat(10, "busy", ticks_a, 100));
        write_file(root / "20" / "stat", pid_stat(20, "hog", ticks_b, 5000));
    }
};

}  // namespace

TEST(KbPerSec, BasicAndEdgeCases) {
    EXPECT_DOUBLE_EQ(kb_per_sec(0, 2048, 2.0), 1.0);
    EXPECT_DOUBLE_EQ(kb_per_sec(5000, 100, 1.0), 0.0);  // counter reset
    EXPECT_DOUBLE_EQ(kb_per_sec(0, 1024, 0.0), 0.0);    // no time elapsed
}

TEST_F(FakeProc, ComputesRatesBetweenTwoReadings) {
    Collector c(ProcReader(root.string()), /*top_n=*/2);  // baseline reading

    // Over 2 seconds: CPU +300 busy / +100 idle, disk +4096 sectors (2 MB),
    // network +20480 rx / +10240 tx bytes.
    write_state(400, 1000, 4096, 20480, 10240, /*ticks_a=*/0, /*ticks_b=*/0);
    MetricSnapshot s = c.sample_with_elapsed(2.0);

    EXPECT_DOUBLE_EQ(s.cpu_percent, 75.0);
    EXPECT_DOUBLE_EQ(s.mem_used_percent, 75.0);
    EXPECT_EQ(s.mem_used_kb, 750000u);
    EXPECT_DOUBLE_EQ(s.disk_read_kbps, 1024.0);   // 4096*512 B / 1024 / 2 s
    EXPECT_DOUBLE_EQ(s.disk_write_kbps, 1024.0);
    EXPECT_DOUBLE_EQ(s.net_rx_kbps, 10.0);
    EXPECT_DOUBLE_EQ(s.net_tx_kbps, 5.0);
    EXPECT_DOUBLE_EQ(s.load.load1, 1.0);
}

TEST_F(FakeProc, RanksTopProcessesByCpuAndMemory) {
    Collector c(ProcReader(root.string()), /*top_n=*/2);

    long hz = sysconf(_SC_CLK_TCK);
    // "busy" uses one full core for 1 s; "hog" uses a quarter core.
    write_state(100, 900, 0, 0, 0, static_cast<int>(hz), static_cast<int>(hz / 4));
    MetricSnapshot s = c.sample_with_elapsed(1.0);

    ASSERT_EQ(s.top_cpu.size(), 2u);
    EXPECT_EQ(s.top_cpu[0].name, "busy");
    EXPECT_NEAR(s.top_cpu[0].cpu_percent, 100.0, 1e-9);
    EXPECT_EQ(s.top_cpu[1].name, "hog");

    ASSERT_EQ(s.top_mem.size(), 2u);
    EXPECT_EQ(s.top_mem[0].name, "hog");  // 5000 pages > 100 pages
}

TEST(RealProc, SmokeTestOnThisMachine) {
    if (!fs::exists("/proc/stat")) GTEST_SKIP() << "not running on Linux";
    Collector c;
    MetricSnapshot s = c.sample_with_elapsed(1.0);
    EXPECT_GE(s.cpu_percent, 0.0);
    EXPECT_LE(s.cpu_percent, 100.0);
    EXPECT_GT(s.mem_total_kb, 0u);
    EXPECT_FALSE(s.top_mem.empty());
}
