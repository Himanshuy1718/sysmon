#include "sysmon/db_writer.hpp"

#include <algorithm>
#include <iostream>

namespace sysmon {

DbWriter::DbWriter(std::shared_ptr<MetricStore> store, DbWriterConfig config)
    : store_(std::move(store)), config_(config), queue_(config.queue_capacity) {}

DbWriter::~DbWriter() { stop(); }

void DbWriter::start() {
    if (started_) return;
    started_ = true;
    thread_ = std::thread(&DbWriter::run, this);
}

void DbWriter::stop() {
    if (!started_) return;
    started_ = false;
    {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        stopping_ = true;
    }
    stop_cv_.notify_all();  // cut short any retry wait
    queue_.close();         // writer drains the rest, then exits
    if (thread_.joinable()) thread_.join();
}

void DbWriter::submit(StoreItem item) { queue_.push(std::move(item)); }

void DbWriter::run() {
    std::vector<StoreItem> batch;
    batch.reserve(config_.max_batch);
    auto batch_started = std::chrono::steady_clock::now();

    while (true) {
        // Wait for the next item. If a batch is open, wait only until it is due.
        std::chrono::milliseconds wait = config_.flush_interval;
        if (!batch.empty()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - batch_started);
            wait = std::max(std::chrono::milliseconds(1), config_.flush_interval - elapsed);
        }
        std::optional<StoreItem> item = queue_.pop_for(wait);

        if (item) {
            if (batch.empty()) batch_started = std::chrono::steady_clock::now();
            batch.push_back(std::move(*item));
        }

        const bool closed_and_empty = !item && queue_.closed();
        const bool full = batch.size() >= config_.max_batch;
        const bool due = !batch.empty() &&
                         std::chrono::steady_clock::now() - batch_started >= config_.flush_interval;

        if (!batch.empty() && (full || due || closed_and_empty)) {
            if (write_with_retry(batch)) {
                written_ += batch.size();
                ++batches_;
            } else {
                lost_at_shutdown_ += batch.size();
            }
            batch.clear();
        }
        if (closed_and_empty) return;
    }
}

bool DbWriter::write_with_retry(const std::vector<StoreItem>& batch) {
    while (true) {
        try {
            store_->write(batch);
            return true;
        } catch (const std::exception& e) {
            ++failures_;
            bool stopping;
            {
                std::lock_guard<std::mutex> lock(stop_mutex_);
                stopping = stopping_;
            }
            if (stopping) {
                std::cerr << "[db] write failed during shutdown, giving up: " << e.what() << "\n";
                return false;
            }
            std::cerr << "[db] write failed (" << batch.size() << " items), retrying in "
                      << config_.retry_delay.count() / 1000.0 << "s: " << e.what() << "\n";
        }
        // Retry the SAME batch after a delay. The transaction rolled back, so
        // nothing was half-written and a retry cannot create duplicates.
        std::unique_lock<std::mutex> lock(stop_mutex_);
        stop_cv_.wait_for(lock, config_.retry_delay, [this] { return stopping_; });
    }
}

}  // namespace sysmon
