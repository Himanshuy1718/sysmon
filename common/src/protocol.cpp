#include "sysmon/protocol.hpp"

#include <chrono>
#include <nlohmann/json.hpp>

namespace sysmon::protocol {

using nlohmann::json;

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

namespace {

json process_list_to_json(const std::vector<ProcessUsage>& list) {
    json arr = json::array();
    for (const auto& p : list) {
        arr.push_back({{"pid", p.pid}, {"name", p.name}, {"cpu", p.cpu_percent}, {"rss_kb", p.rss_kb}});
    }
    return arr;
}

std::vector<ProcessUsage> process_list_from_json(const json& arr) {
    std::vector<ProcessUsage> out;
    for (const auto& j : arr) {
        out.push_back({j.at("pid").get<int>(), j.at("name").get<std::string>(),
                       j.at("cpu").get<double>(), j.at("rss_kb").get<std::uint64_t>()});
    }
    return out;
}

json to_json_obj(const MetricSnapshot& s) {
    return {
        {"host", s.host},
        {"ts", s.timestamp_ms},
        {"cpu", s.cpu_percent},
        {"mem_pct", s.mem_used_percent},
        {"mem_used_kb", s.mem_used_kb},
        {"mem_total_kb", s.mem_total_kb},
        {"disk_read_kbps", s.disk_read_kbps},
        {"disk_write_kbps", s.disk_write_kbps},
        {"net_rx_kbps", s.net_rx_kbps},
        {"net_tx_kbps", s.net_tx_kbps},
        {"load", {s.load.load1, s.load.load5, s.load.load15}},
        {"top_cpu", process_list_to_json(s.top_cpu)},
        {"top_mem", process_list_to_json(s.top_mem)},
    };
}

MetricSnapshot from_json_obj(const json& j) {
    MetricSnapshot s;
    s.host = j.at("host").get<std::string>();
    s.timestamp_ms = j.at("ts").get<std::int64_t>();
    s.cpu_percent = j.at("cpu").get<double>();
    s.mem_used_percent = j.at("mem_pct").get<double>();
    s.mem_used_kb = j.at("mem_used_kb").get<std::uint64_t>();
    s.mem_total_kb = j.at("mem_total_kb").get<std::uint64_t>();
    s.disk_read_kbps = j.at("disk_read_kbps").get<double>();
    s.disk_write_kbps = j.at("disk_write_kbps").get<double>();
    s.net_rx_kbps = j.at("net_rx_kbps").get<double>();
    s.net_tx_kbps = j.at("net_tx_kbps").get<double>();
    const auto& load = j.at("load");
    s.load = {load.at(0).get<double>(), load.at(1).get<double>(), load.at(2).get<double>()};
    s.top_cpu = process_list_from_json(j.at("top_cpu"));
    s.top_mem = process_list_from_json(j.at("top_mem"));
    return s;
}

}  // namespace

std::string snapshot_to_json(const MetricSnapshot& s) { return to_json_obj(s).dump(); }

std::string processes_to_json(const std::vector<ProcessUsage>& list) {
    return process_list_to_json(list).dump();
}

MetricSnapshot snapshot_from_json(const std::string& text) {
    try {
        return from_json_obj(json::parse(text));
    } catch (const json::exception& e) {
        throw ProtocolError(std::string("bad snapshot: ") + e.what());
    }
}

std::string encode_hello(const std::string& host) {
    return json{{"type", "hello"}, {"host", host}, {"version", kVersion}}.dump();
}

std::string encode_metrics(const MetricSnapshot& s) {
    return json{{"type", "metrics"}, {"host", s.host}, {"data", to_json_obj(s)}}.dump();
}

std::string encode_heartbeat(const std::string& host, std::int64_t ts) {
    return json{{"type", "heartbeat"}, {"host", host}, {"ts", ts}}.dump();
}

Message decode(const std::string& payload) {
    try {
        json j = json::parse(payload);
        Message m;
        const std::string type = j.at("type").get<std::string>();
        m.host = j.at("host").get<std::string>();
        if (m.host.empty()) throw ProtocolError("empty host");

        if (type == "hello") {
            m.type = MessageType::Hello;
            m.version = j.at("version").get<int>();
        } else if (type == "metrics") {
            m.type = MessageType::Metrics;
            m.snapshot = from_json_obj(j.at("data"));
            m.timestamp_ms = m.snapshot.timestamp_ms;
        } else if (type == "heartbeat") {
            m.type = MessageType::Heartbeat;
            m.timestamp_ms = j.at("ts").get<std::int64_t>();
        } else {
            throw ProtocolError("unknown message type: " + type);
        }
        return m;
    } catch (const json::exception& e) {
        // Covers invalid JSON, missing fields and wrong field types.
        throw ProtocolError(std::string("bad message: ") + e.what());
    }
}

}  // namespace sysmon::protocol
