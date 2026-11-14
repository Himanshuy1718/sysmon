#pragma once
// DbWriter: moves database work OFF the network threads.
//
//   handler threads ──submit()──► [ bounded queue ] ──► writer thread ──► MetricStore
//                                                        (batches + retries)
//
// Why not insert directly from each handler thread?
//  - A database write takes milliseconds; a slow or down database would then
//    stall every agent's connection. With a queue, network threads just
//    enqueue (microseconds) and move on: the same producer–consumer idea as
//    the agent, reusing the same ThreadSafeQueue.
//  - BATCHING: one transaction with 200 rows is far faster than 200
//    transactions with 1 row, because each COMMIT waits for the disk.
//    The writer flushes when a batch is full OR after flush_interval,
//    whichever comes first — so data is never stuck waiting long.
//  - RETRIES: if the database is down, the writer keeps the failed batch and
//    retries it. Meanwhile the queue buffers new data and, if full, drops the
//    oldest — memory stays bounded.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "sysmon/store.hpp"
#include "sysmon/thread_safe_queue.hpp"

namespace sysmon {

struct DbWriterConfig {
    std::size_t queue_capacity = 50000;               // items buffered while DB is slow/down
    std::size_t max_batch = 500;                      // rows per transaction
    std::chrono::milliseconds flush_interval{1000};   // max delay before a partial batch is written
    std::chrono::milliseconds retry_delay{2000};      // wait after a failed write
};

class DbWriter {
public:
    DbWriter(std::shared_ptr<MetricStore> store, DbWriterConfig config = {});
    ~DbWriter();  // stop()

    DbWriter(const DbWriter&) = delete;
    DbWriter& operator=(const DbWriter&) = delete;

    void start();
    // Stops accepting work, writes what is queued (one final attempt), joins.
    void stop();

    // Thread-safe, never blocks on the database.
    void submit(StoreItem item);

    std::uint64_t written() const { return written_.load(); }        // items committed
    std::uint64_t batches() const { return batches_.load(); }        // successful transactions
    std::uint64_t failures() const { return failures_.load(); }      // failed write attempts
    std::uint64_t dropped() const { return queue_.dropped(); }       // lost: queue overflow
    std::uint64_t lost_at_shutdown() const { return lost_at_shutdown_.load(); }
    std::size_t pending() const { return queue_.size(); }

private:
    void run();
    // Tries to write until success or stop. Returns true if written.
    bool write_with_retry(const std::vector<StoreItem>& batch);

    std::shared_ptr<MetricStore> store_;
    DbWriterConfig config_;
    ThreadSafeQueue<StoreItem> queue_;
    std::thread thread_;
    bool started_ = false;

    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    bool stopping_ = false;

    std::atomic<std::uint64_t> written_{0};
    std::atomic<std::uint64_t> batches_{0};
    std::atomic<std::uint64_t> failures_{0};
    std::atomic<std::uint64_t> lost_at_shutdown_{0};
};

}  // namespace sysmon
