#pragma once
// Application protocol between agent and server. Each frame's payload is a
// JSON object with a "type" field:
//
//   {"type":"hello",     "host":"node-1", "version":1}         first message on every connection
//   {"type":"metrics",   "host":"node-1", "data":{...snapshot...}}
//   {"type":"heartbeat", "host":"node-1", "ts":1794553548970}  "I'm alive" when idle
//
// JSON is human-readable and easy to debug. A binary format (e.g. Protocol
// Buffers) would be smaller and faster: a reasonable future improvement.

#include <cstdint>
#include <stdexcept>
#include <string>

#include "sysmon/metrics.hpp"

namespace sysmon::protocol {

constexpr int kVersion = 1;

struct ProtocolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class MessageType { Hello, Metrics, Heartbeat };

struct Message {
    MessageType type = MessageType::Hello;
    std::string host;
    std::int64_t timestamp_ms = 0;
    int version = 0;          // hello only
    MetricSnapshot snapshot;  // metrics only
};

std::string encode_hello(const std::string& host);
std::string encode_metrics(const MetricSnapshot& snapshot);
std::string encode_heartbeat(const std::string& host, std::int64_t timestamp_ms);

// Throws ProtocolError on invalid JSON or missing/wrong fields.
Message decode(const std::string& payload);

// Snapshot <-> JSON text (also used later for storage and the API).
std::string snapshot_to_json(const MetricSnapshot& s);
MetricSnapshot snapshot_from_json(const std::string& json);

std::int64_t now_ms();

}  // namespace sysmon::protocol
