#include <gtest/gtest.h>

#include "sysmon/protocol.hpp"

using namespace sysmon;
using namespace sysmon::protocol;

namespace {
MetricSnapshot sample_snapshot() {
    MetricSnapshot s;
    s.host = "node-1";
    s.timestamp_ms = 1794553548970;
    s.cpu_percent = 42.5;
    s.mem_used_percent = 61.25;
    s.mem_used_kb = 5000000;
    s.mem_total_kb = 8000000;
    s.disk_read_kbps = 10.5;
    s.disk_write_kbps = 20.25;
    s.net_rx_kbps = 1.5;
    s.net_tx_kbps = 2.5;
    s.load = {0.5, 0.75, 1.0};
    s.top_cpu = {{101, "nginx", 35.5, 2048}, {7, "Web Content", 10.0, 4096}};
    s.top_mem = {{7, "Web Content", 10.0, 4096}};
    return s;
}
}  // namespace

TEST(Protocol, SnapshotRoundTripsThroughJson) {
    MetricSnapshot in = sample_snapshot();
    MetricSnapshot out = snapshot_from_json(snapshot_to_json(in));
    EXPECT_EQ(out.host, in.host);
    EXPECT_EQ(out.timestamp_ms, in.timestamp_ms);
    EXPECT_DOUBLE_EQ(out.cpu_percent, in.cpu_percent);
    EXPECT_EQ(out.mem_total_kb, in.mem_total_kb);
    EXPECT_DOUBLE_EQ(out.load.load15, 1.0);
    ASSERT_EQ(out.top_cpu.size(), 2u);
    EXPECT_EQ(out.top_cpu[1].name, "Web Content");
    EXPECT_EQ(out.top_cpu[0].rss_kb, 2048u);
}

TEST(Protocol, DecodesEachMessageType) {
    Message hello = decode(encode_hello("node-1"));
    EXPECT_EQ(hello.type, MessageType::Hello);
    EXPECT_EQ(hello.host, "node-1");
    EXPECT_EQ(hello.version, kVersion);

    Message metrics = decode(encode_metrics(sample_snapshot()));
    EXPECT_EQ(metrics.type, MessageType::Metrics);
    EXPECT_DOUBLE_EQ(metrics.snapshot.cpu_percent, 42.5);

    Message hb = decode(encode_heartbeat("node-1", 123));
    EXPECT_EQ(hb.type, MessageType::Heartbeat);
    EXPECT_EQ(hb.timestamp_ms, 123);
}

TEST(Protocol, RejectsBadMessages) {
    EXPECT_THROW(decode("not json at all"), ProtocolError);
    EXPECT_THROW(decode(R"({"host":"x"})"), ProtocolError);                       // no type
    EXPECT_THROW(decode(R"({"type":"explode","host":"x"})"), ProtocolError);      // unknown type
    EXPECT_THROW(decode(R"({"type":"hello","host":""})"), ProtocolError);         // empty host
    EXPECT_THROW(decode(R"({"type":"metrics","host":"x","data":{}})"), ProtocolError);  // missing fields
    EXPECT_THROW(decode(R"({"type":"hello","host":5,"version":1})"), ProtocolError);    // wrong type
}
