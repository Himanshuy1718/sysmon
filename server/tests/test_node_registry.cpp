#include <gtest/gtest.h>

#include "sysmon/node_registry.hpp"

using namespace std::chrono_literals;
using namespace sysmon;

TEST(NodeRegistry, FirstContactMarksNodeUp) {
    NodeRegistry r;
    auto t0 = Clock::now();
    EXPECT_TRUE(r.touch("node-1", "10.0.0.1:5000", t0));   // new -> came UP
    EXPECT_FALSE(r.touch("node-1", "10.0.0.1:5000", t0));  // already up
    auto n = r.get("node-1");
    ASSERT_TRUE(n);
    EXPECT_TRUE(n->up);
    EXPECT_EQ(n->messages, 2u);
}

TEST(NodeRegistry, SilentNodeGoesDownOnceThenComesBack) {
    NodeRegistry r;
    auto t0 = Clock::now();
    r.touch("node-1", "a", t0);
    r.touch("node-2", "b", t0 + 10s);

    // At t0+16s with a 15 s timeout: node-1 is stale, node-2 is not.
    auto down = r.mark_stale(t0 + 16s, 15s);
    ASSERT_EQ(down.size(), 1u);
    EXPECT_EQ(down[0], "node-1");
    EXPECT_FALSE(r.get("node-1")->up);
    EXPECT_TRUE(r.get("node-2")->up);

    // Reported only once, not on every check.
    EXPECT_TRUE(r.mark_stale(t0 + 17s, 15s).empty());

    // A new message brings it back UP.
    EXPECT_TRUE(r.touch("node-1", "a", t0 + 20s));
    EXPECT_TRUE(r.get("node-1")->up);
}

TEST(NodeRegistry, KeepsLatestSnapshot) {
    NodeRegistry r;
    MetricSnapshot s;
    s.host = "node-1";
    s.cpu_percent = 12.5;
    r.touch("node-1", "a", Clock::now(), &s);
    r.touch("node-1", "a", Clock::now());  // heartbeat: snapshot unchanged
    ASSERT_TRUE(r.get("node-1")->last_snapshot);
    EXPECT_DOUBLE_EQ(r.get("node-1")->last_snapshot->cpu_percent, 12.5);
    EXPECT_FALSE(r.get("unknown"));
}
