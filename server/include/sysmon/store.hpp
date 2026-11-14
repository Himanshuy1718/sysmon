#pragma once
// Storage abstraction.
//
// The DbWriter (batching, retries) talks to this interface, not to
// PostgreSQL directly. That lets the batching logic be unit-tested with a
// fake in-memory store, and the real PgStore be tested separately.

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "sysmon/metrics.hpp"

namespace sysmon {

struct NodeEvent {
    std::string host;
    std::string address;
    bool up = true;
    std::int64_t timestamp_ms = 0;
};

// One thing to persist: a metrics snapshot or an UP/DOWN event.
using StoreItem = std::variant<MetricSnapshot, NodeEvent>;

class MetricStore {
public:
    virtual ~MetricStore() = default;

    // Writes all items ATOMICALLY: either every item is saved or none is
    // (one database transaction). Throws on failure so the caller can retry
    // the same batch without creating duplicates.
    virtual void write(const std::vector<StoreItem>& batch) = 0;
};

}  // namespace sysmon
