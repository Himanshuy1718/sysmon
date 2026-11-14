// PgStore tests against a REAL PostgreSQL database.
// Run with:  SYSMON_TEST_DB=postgresql://sysmon:sysmon@localhost:5432/sysmon ctest
// Without SYSMON_TEST_DB they are skipped.
//
// Each test uses its own PostgreSQL SCHEMA (a namespace inside the database),
// so tests never touch the real tables and can be dropped cleanly afterwards.

#include <gtest/gtest.h>

#include <cstdlib>
#include <random>

#include "sysmon/pg_store.hpp"

using namespace sysmon;

namespace {

class PgStoreTest : public ::testing::Test {
protected:
    std::string base_conninfo;
    std::string schema;
    std::unique_ptr<PgStore> store;

    void SetUp() override {
        const char* db = std::getenv("SYSMON_TEST_DB");
        if (!db || !*db) GTEST_SKIP() << "set SYSMON_TEST_DB to run PostgreSQL tests";
        base_conninfo = db;

        std::random_device rd;
        schema = "sysmon_test_" + std::to_string(rd() % 1000000000);
        {
            PgStore admin(base_conninfo);
            admin.exec("CREATE SCHEMA " + schema);
        }
        // search_path makes every unqualified table name resolve to our test schema.
        std::string sep = base_conninfo.find('?') == std::string::npos ? "?" : "&";
        store = std::make_unique<PgStore>(base_conninfo + sep + "options=-csearch_path%3D" + schema);
        store->ensure_ready();
    }

    void TearDown() override {
        store.reset();
        if (!schema.empty() && !base_conninfo.empty()) {
            PgStore admin(base_conninfo);
            admin.exec("DROP SCHEMA IF EXISTS " + schema + " CASCADE");
        }
    }

    static MetricSnapshot snap(const std::string& host, std::int64_t ts, double cpu) {
        MetricSnapshot s;
        s.host = host;
        s.timestamp_ms = ts;
        s.cpu_percent = cpu;
        s.mem_used_percent = 50;
        s.mem_used_kb = 1000;
        s.mem_total_kb = 2000;
        s.load = {0.5, 0.25, 0.125};
        s.top_cpu = {{42, "nginx", 33.5, 1024}, {7, "Web \"Content\"", 1.0, 2048}};
        return s;
    }
};

}  // namespace

TEST_F(PgStoreTest, CreatesSchemaAndIsIdempotent) {
    EXPECT_EQ(store->query_int("SELECT count(*) FROM metrics"), 0);
    store->ensure_ready();  // second run must not fail
    PgStore again(base_conninfo + (base_conninfo.find('?') == std::string::npos ? "?" : "&") +
                  "options=-csearch_path%3D" + schema);
    again.ensure_ready();   // fresh connection applying the schema again
}

TEST_F(PgStoreTest, WritesMetricsAndNodes) {
    store->write({snap("node-1", 1794553548000, 12.5), snap("node-1", 1794553549000, 50.0),
                  snap("node-2", 1794553549000, 99.0)});

    EXPECT_EQ(store->query_int("SELECT count(*) FROM metrics"), 3);
    EXPECT_EQ(store->query_int("SELECT count(*) FROM nodes"), 2);
    EXPECT_EQ(store->query_int("SELECT count(*) FROM metrics WHERE host='node-1'"), 2);
    // Timestamp converted from epoch milliseconds correctly.
    EXPECT_EQ(store->query_int(
                  "SELECT (extract(epoch FROM max(ts)) * 1000)::bigint FROM metrics WHERE host='node-1'"),
              1794553549000);
    // last_seen is the newest sample, not whichever row was written last.
    EXPECT_EQ(store->query_int(
                  "SELECT (extract(epoch FROM last_seen) * 1000)::bigint FROM nodes WHERE host='node-1'"),
              1794553549000);
}

TEST_F(PgStoreTest, TopProcessesStoredAsQueryableJson) {
    store->write({snap("node-1", 1000, 10)});
    // JSONB lets SQL look inside the array: name of the top CPU process.
    EXPECT_EQ(store->query_int(
                  "SELECT (top_cpu->0->>'pid')::int FROM metrics WHERE top_cpu->0->>'name' = 'nginx'"),
              42);
    EXPECT_EQ(store->query_int("SELECT jsonb_array_length(top_cpu) FROM metrics"), 2);
}

TEST_F(PgStoreTest, RecordsUpDownEventsAndStatus) {
    store->write({NodeEvent{"node-1", "10.0.0.5:4000", true, 1000}});
    store->write({NodeEvent{"node-1", "", false, 2000}});

    EXPECT_EQ(store->query_int("SELECT count(*) FROM node_events WHERE host='node-1'"), 2);
    EXPECT_EQ(store->query_int("SELECT count(*) FROM nodes WHERE host='node-1' AND status='DOWN'"), 1);
    // Address from the UP event is kept when the DOWN event has none.
    EXPECT_EQ(store->query_int("SELECT count(*) FROM nodes WHERE address='10.0.0.5:4000'"), 1);
}

TEST_F(PgStoreTest, SqlInjectionIsStoredAsPlainText) {
    const std::string evil = "x'); DROP TABLE metrics; --";
    store->write({snap(evil, 1000, 1)});
    // Table still exists and the name was stored literally.
    EXPECT_EQ(store->query_int("SELECT count(*) FROM metrics"), 1);
    EXPECT_EQ(store->query_int("SELECT count(*) FROM nodes WHERE host = 'x''); DROP TABLE metrics; --'"), 1);
}

TEST_F(PgStoreTest, FailedBatchIsRolledBackCompletely) {
    store->write({snap("node-1", 1000, 1)});  // 1 row committed
    // Add a rule that a cpu value of 500 will break, to force a mid-batch failure.
    store->exec("ALTER TABLE metrics ADD CONSTRAINT cpu_max CHECK (cpu_pct <= 100)");

    // Middle item violates the constraint -> the whole batch must be rolled back.
    EXPECT_THROW(store->write({snap("node-1", 2000, 10), snap("node-1", 3000, 500),
                               snap("node-1", 4000, 20)}),
                 std::exception);
    EXPECT_EQ(store->query_int("SELECT count(*) FROM metrics"), 1);  // still just the first row

    // And the connection is still usable afterwards.
    store->write({snap("node-1", 5000, 30)});
    EXPECT_EQ(store->query_int("SELECT count(*) FROM metrics"), 2);
}

TEST(PgStore, UnreachableDatabaseThrowsInsteadOfCrashing) {
    PgStore store("postgresql://nobody:wrong@127.0.0.1:1/none?connect_timeout=2");
    EXPECT_THROW(store.ensure_ready(), std::exception);
    EXPECT_THROW(store.write({MetricSnapshot{}}), std::exception);
}
