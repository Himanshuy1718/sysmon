#pragma once
// PostgreSQL implementation of MetricStore, using libpq (the official C client).

#include <memory>
#include <string>

#include "sysmon/store.hpp"

struct pg_conn;  // libpq's connection type (PGconn); keeps libpq out of this header

namespace sysmon {

class PgStore : public MetricStore {
public:
    // conninfo, e.g. "postgresql://sysmon:sysmon@localhost:5432/sysmon".
    // Does not connect yet: connection is made (and re-made) on demand, so a
    // database that is down at startup is not fatal.
    explicit PgStore(std::string conninfo);
    ~PgStore() override;

    PgStore(const PgStore&) = delete;
    PgStore& operator=(const PgStore&) = delete;

    void write(const std::vector<StoreItem>& batch) override;

    // Connects (if needed) and creates the tables. Throws on failure.
    void ensure_ready();

    // Runs any SQL (used by tests and tools). Throws on failure.
    void exec(const std::string& sql);
    // Runs a query that returns one number, e.g. "SELECT count(*) FROM metrics".
    long long query_int(const std::string& sql);

private:
    void connect();
    void prepare_statements();

    std::string conninfo_;
    pg_conn* conn_ = nullptr;
    bool schema_ready_ = false;  // tables exist (true once per process)
    bool prepared_ = false;      // statements prepared on THIS connection
};

}  // namespace sysmon
