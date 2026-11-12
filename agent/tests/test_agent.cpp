// Tests the two-thread Agent on the real /proc with tiny intervals.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "sysmon/agent.hpp"

using namespace std::chrono_literals;
using namespace sysmon;

namespace {
bool have_proc() { return std::filesystem::exists("/proc/stat"); }

// Polls until cond() is true or timeout passes.
template <typename Cond>
bool wait_until(Cond cond, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return cond();
}
}  // namespace

TEST(Agent, DeliversSnapshotsInOrder) {
    if (!have_proc()) GTEST_SKIP();
    std::mutex m;
    std::vector<int64_t> timestamps;

    Agent agent(Collector{}, [&](const MetricSnapshot& s) {
        std::lock_guard<std::mutex> lock(m);
        timestamps.push_back(s.timestamp_ms);
    }, AgentConfig{0.02, 100});

    agent.start();
    ASSERT_TRUE(wait_until([&] { return agent.sent() >= 5; }, 3000ms));
    agent.stop();

    // Everything collected was sent: stop() drains the queue.
    EXPECT_EQ(agent.collected(), agent.sent());
    std::lock_guard<std::mutex> lock(m);
    for (std::size_t i = 1; i < timestamps.size(); ++i) {
        EXPECT_LE(timestamps[i - 1], timestamps[i]);  // FIFO order preserved
    }
}

TEST(Agent, StopIsPromptEvenWithLongInterval) {
    if (!have_proc()) GTEST_SKIP();
    Agent agent(Collector{}, [](const MetricSnapshot&) {}, AgentConfig{60.0, 10});
    agent.start();
    std::this_thread::sleep_for(50ms);

    auto start = std::chrono::steady_clock::now();
    agent.stop();  // collector is mid-sleep for 60 s; condition_variable must wake it
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
    EXPECT_EQ(agent.collected(), 0u);
}

// The key reason for two threads: a slow sender must not slow down collection.
TEST(Agent, SlowSinkDoesNotBlockCollection) {
    if (!have_proc()) GTEST_SKIP();
    Agent agent(Collector{}, [](const MetricSnapshot&) { std::this_thread::sleep_for(200ms); },
                AgentConfig{0.01, 5});
    agent.start();
    std::this_thread::sleep_for(500ms);
    // Snapshot counters BEFORE stop(), because stop() drains the queue.
    auto collected = agent.collected();
    auto sent = agent.sent();
    agent.stop();

    EXPECT_GT(collected, 10u);     // collector kept its ~10 ms rhythm
    EXPECT_LE(sent, 4u);           // sender managed only a few in 500 ms
    EXPECT_GT(agent.dropped(), 0u);  // bounded queue dropped the oldest
}

TEST(Agent, SinkExceptionDoesNotKillSender) {
    if (!have_proc()) GTEST_SKIP();
    std::atomic<int> calls{0};
    Agent agent(Collector{}, [&](const MetricSnapshot&) {
        if (++calls == 1) throw std::runtime_error("simulated network error");
    }, AgentConfig{0.02, 100});

    agent.start();
    ASSERT_TRUE(wait_until([&] { return agent.sent() >= 3; }, 3000ms));
    agent.stop();
    EXPECT_GE(agent.errors(), 1u);
}

TEST(Agent, DestructorStopsThreads) {
    if (!have_proc()) GTEST_SKIP();
    {
        Agent agent(Collector{}, [](const MetricSnapshot&) {}, AgentConfig{0.01, 10});
        agent.start();
        std::this_thread::sleep_for(30ms);
    }  // no explicit stop(): RAII must join both threads, or std::terminate is called
    SUCCEED();
}
