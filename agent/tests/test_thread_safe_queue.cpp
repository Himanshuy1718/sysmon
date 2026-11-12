#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <numeric>
#include <thread>
#include <vector>

#include "sysmon/thread_safe_queue.hpp"

using namespace std::chrono_literals;
using sysmon::ThreadSafeQueue;

TEST(ThreadSafeQueue, IsFifo) {
    ThreadSafeQueue<int> q(10);
    for (int i = 1; i <= 3; ++i) q.push(i);
    EXPECT_EQ(*q.pop(), 1);
    EXPECT_EQ(*q.pop(), 2);
    EXPECT_EQ(*q.pop(), 3);
}

TEST(ThreadSafeQueue, DropsOldestWhenFull) {
    ThreadSafeQueue<int> q(3);
    for (int i = 1; i <= 5; ++i) q.push(i);  // 1 and 2 get dropped
    EXPECT_EQ(q.size(), 3u);
    EXPECT_EQ(q.dropped(), 2u);
    EXPECT_EQ(*q.pop(), 3);
}

TEST(ThreadSafeQueue, PopBlocksUntilAnItemArrives) {
    ThreadSafeQueue<int> q(10);
    auto start = std::chrono::steady_clock::now();
    std::thread producer([&] {
        std::this_thread::sleep_for(100ms);
        q.push(42);
    });
    auto v = q.pop();  // must sleep here until the producer pushes
    auto waited = std::chrono::steady_clock::now() - start;
    producer.join();

    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
    EXPECT_GE(waited, 90ms);
}

TEST(ThreadSafeQueue, CloseWakesBlockedConsumer) {
    ThreadSafeQueue<int> q(10);
    std::optional<int> result = 0;
    std::thread consumer([&] { result = q.pop(); });
    std::this_thread::sleep_for(50ms);
    q.close();
    consumer.join();  // would hang forever if close() did not wake it
    EXPECT_FALSE(result.has_value());
}

TEST(ThreadSafeQueue, CloseStillDrainsQueuedItems) {
    ThreadSafeQueue<int> q(10);
    q.push(1);
    q.push(2);
    q.close();
    EXPECT_FALSE(q.push(3));  // rejected after close
    EXPECT_EQ(*q.pop(), 1);
    EXPECT_EQ(*q.pop(), 2);
    EXPECT_FALSE(q.pop().has_value());  // closed and empty -> done
}

TEST(ThreadSafeQueue, PopForTimesOut) {
    ThreadSafeQueue<int> q(10);
    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(q.pop_for(50ms).has_value());
    EXPECT_GE(std::chrono::steady_clock::now() - start, 45ms);
}

// Stress test: 4 producers and 4 consumers hammer the queue at once.
// Every item must be received exactly once — no loss, no duplicates.
TEST(ThreadSafeQueue, ManyProducersManyConsumers) {
    constexpr int kProducers = 4, kConsumers = 4, kPerProducer = 20000;
    ThreadSafeQueue<int> q(kProducers * kPerProducer);  // large enough: no drops

    std::atomic<long long> sum{0};
    std::atomic<int> received{0};

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            while (auto v = q.pop()) {
                sum += *v;
                ++received;
            }
        });
    }
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 1; i <= kPerProducer; ++i) q.push(p * kPerProducer + i);
        });
    }
    for (auto& t : producers) t.join();
    q.close();
    for (auto& t : consumers) t.join();

    const long long n = static_cast<long long>(kProducers) * kPerProducer;
    EXPECT_EQ(received.load(), n);
    EXPECT_EQ(sum.load(), n * (n + 1) / 2);  // 1 + 2 + ... + n
    EXPECT_EQ(q.dropped(), 0u);
}
