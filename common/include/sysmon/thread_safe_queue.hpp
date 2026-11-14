#pragma once
// A bounded, blocking, thread-safe FIFO queue — the classic producer–consumer
// building block.
//
//   producer thread  --push()-->  [ queue ]  --pop()-->  consumer thread
//
// - std::mutex protects the deque: only one thread touches it at a time.
// - std::condition_variable lets a consumer SLEEP while the queue is empty
//   instead of spinning in a loop and burning CPU ("busy waiting").
// - Bounded: when full, the OLDEST item is dropped. For monitoring, fresh
//   data matters more than stale data, and the agent must never grow its
//   memory without limit if the server is slow or down.
// - close(): wakes every waiting consumer so threads can shut down cleanly.
//   Items already in the queue are still handed out (drained) after close.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace sysmon {

template <typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(std::size_t capacity) : capacity_(capacity ? capacity : 1) {}

    // Not copyable: copying a mutex makes no sense.
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    // Adds an item. Returns false if the queue is closed.
    // If full, drops the oldest item to make room.
    bool push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return false;
            if (queue_.size() >= capacity_) {
                queue_.pop_front();
                ++dropped_;
            }
            queue_.push_back(std::move(item));
        }  // unlock BEFORE notify, so the woken thread can grab the lock at once
        not_empty_.notify_one();
        return true;
    }

    // Blocks until an item is available. Returns nullopt only when the queue
    // is closed AND empty — that is the consumer's signal to exit.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        // The predicate form re-checks the condition after every wake-up,
        // which protects against "spurious wake-ups".
        not_empty_.wait(lock, [this] { return !queue_.empty() || closed_; });
        return take_front_locked();
    }

    // Like pop(), but gives up after `timeout`.
    std::optional<T> pop_for(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; });
        return take_front_locked();
    }

    // No more pushes accepted; all blocked consumers wake up.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    std::uint64_t dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }
    std::size_t capacity() const { return capacity_; }

private:
    // Caller must hold mutex_.
    std::optional<T> take_front_locked() {
        if (queue_.empty()) return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;  // mutable: locked even in const getters
    std::condition_variable not_empty_;
    std::deque<T> queue_;
    bool closed_ = false;
    std::uint64_t dropped_ = 0;
};

}  // namespace sysmon
