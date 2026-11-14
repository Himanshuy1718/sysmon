// DbWriter tests with a FAKE in-memory store: batching, flushing and retry
// logic are checked without needing a database.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "sysmon/db_writer.hpp"

using namespace std::chrono_literals;
using namespace sysmon;

namespace {

class FakeStore : public MetricStore {
public:
    void write(const std::vector<StoreItem>& batch) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(write_delay_ms.load()));
        std::lock_guard<std::mutex> lock(m);
        ++attempts;
        if (fail_next > 0) {
            --fail_next;
            throw std::runtime_error("simulated: database down");
        }
        batch_sizes.push_back(batch.size());
        items.insert(items.end(), batch.begin(), batch.end());
    }
    std::size_t item_count() {
        std::lock_guard<std::mutex> lock(m);
        return items.size();
    }

    std::mutex m;
    std::vector<StoreItem> items;
    std::vector<std::size_t> batch_sizes;
    int attempts = 0;
    int fail_next = 0;
    std::atomic<int> write_delay_ms{0};
};

MetricSnapshot snap(int i) {
    MetricSnapshot s;
    s.host = "node-1";
    s.timestamp_ms = i;
    s.cpu_percent = i;
    return s;
}

template <typename Cond>
bool wait_until(Cond cond, std::chrono::milliseconds timeout = 3000ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return cond();
}

}  // namespace

TEST(DbWriter, GroupsItemsIntoBatches) {
    auto store = std::make_shared<FakeStore>();
    DbWriterConfig cfg;
    cfg.max_batch = 10;
    cfg.flush_interval = 5000ms;  // long: only "batch full" should trigger writes
    DbWriter writer(store, cfg);
    for (int i = 0; i < 30; ++i) writer.submit(snap(i));  // queued before the thread starts
    writer.start();

    ASSERT_TRUE(wait_until([&] { return store->item_count() == 30; }));
    writer.stop();
    std::lock_guard<std::mutex> lock(store->m);
    EXPECT_EQ(store->batch_sizes, (std::vector<std::size_t>{10, 10, 10}));
    EXPECT_EQ(writer.batches(), 3u);
}

TEST(DbWriter, FlushesPartialBatchAfterInterval) {
    auto store = std::make_shared<FakeStore>();
    DbWriterConfig cfg;
    cfg.max_batch = 1000;
    cfg.flush_interval = 100ms;
    DbWriter writer(store, cfg);
    writer.start();
    writer.submit(snap(1));
    writer.submit(snap(2));

    // Batch is far from full, but must still be written within ~100 ms.
    auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(wait_until([&] { return store->item_count() == 2; }, 2000ms));
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1000ms);
}

TEST(DbWriter, KeepsOrderAcrossBatches) {
    auto store = std::make_shared<FakeStore>();
    DbWriterConfig cfg;
    cfg.max_batch = 7;
    cfg.flush_interval = 20ms;
    DbWriter writer(store, cfg);
    writer.start();
    for (int i = 0; i < 50; ++i) writer.submit(snap(i));
    writer.stop();  // flushes the rest

    std::lock_guard<std::mutex> lock(store->m);
    ASSERT_EQ(store->items.size(), 50u);
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(std::get<MetricSnapshot>(store->items[i]).timestamp_ms, i);
    }
}

TEST(DbWriter, RetriesTheSameBatchWhenDatabaseFails) {
    auto store = std::make_shared<FakeStore>();
    store->fail_next = 3;  // database "down" for the first 3 attempts
    DbWriterConfig cfg;
    cfg.flush_interval = 10ms;
    cfg.retry_delay = 20ms;
    DbWriter writer(store, cfg);
    writer.start();
    writer.submit(snap(1));
    writer.submit(NodeEvent{"node-1", "1.2.3.4:5", true, 1});

    ASSERT_TRUE(wait_until([&] { return store->item_count() == 2; }));
    EXPECT_EQ(writer.failures(), 3u);
    EXPECT_EQ(writer.written(), 2u);  // nothing lost, nothing duplicated
    std::lock_guard<std::mutex> lock(store->m);
    EXPECT_EQ(store->attempts, 4);
}

TEST(DbWriter, SubmitNeverBlocksOnSlowDatabase) {
    auto store = std::make_shared<FakeStore>();
    store->write_delay_ms = 300;  // very slow database
    DbWriterConfig cfg;
    cfg.max_batch = 1;
    cfg.flush_interval = 1ms;
    DbWriter writer(store, cfg);
    writer.start();

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) writer.submit(snap(i));
    // 100 submits while each write takes 300 ms: submit must return at once.
    EXPECT_LT(std::chrono::steady_clock::now() - start, 100ms);
    store->write_delay_ms = 0;  // speed the database up so stop() can flush quickly
    writer.stop();
    EXPECT_EQ(writer.written(), 100u);
}

TEST(DbWriter, BoundedQueueDropsOldestWhenDatabaseIsDown) {
    auto store = std::make_shared<FakeStore>();
    store->fail_next = 1000000;  // database never comes back
    DbWriterConfig cfg;
    cfg.queue_capacity = 10;
    cfg.max_batch = 5;
    cfg.flush_interval = 1ms;
    cfg.retry_delay = 50ms;
    DbWriter writer(store, cfg);
    writer.start();
    for (int i = 0; i < 100; ++i) writer.submit(snap(i));

    EXPECT_GT(writer.dropped(), 0u);   // memory stayed bounded
    EXPECT_LE(writer.pending(), 10u);
    auto start = std::chrono::steady_clock::now();
    writer.stop();  // must not hang retrying forever
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
    EXPECT_EQ(writer.written(), 0u);
    EXPECT_GT(writer.lost_at_shutdown(), 0u);
}
