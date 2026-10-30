#include <gtest/gtest.h>

#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

#include "ota/bounded_queue.hpp"

using ota::BoundedQueue;

TEST(BoundedQueue, RejectsZeroCapacity) {
    EXPECT_THROW(BoundedQueue<int>(0), std::invalid_argument);
}

TEST(BoundedQueue, FifoOrder) {
    BoundedQueue<int> q(8);
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(q.push(i));
    for (int i = 0; i < 5; ++i) EXPECT_EQ(q.pop().value(), i);
}

TEST(BoundedQueue, TryPushFailsWhenFull) {
    BoundedQueue<int> q(2);
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_FALSE(q.try_push(3));
    EXPECT_EQ(q.high_watermark(), 2u);
}

TEST(BoundedQueue, CloseDrainsRemainingItemsThenReturnsNullopt) {
    BoundedQueue<int> q(4);
    q.push(7);
    q.push(8);
    q.close();
    EXPECT_FALSE(q.push(9));
    EXPECT_EQ(q.pop().value(), 7);
    EXPECT_EQ(q.pop().value(), 8);
    EXPECT_FALSE(q.pop().has_value());
}

TEST(BoundedQueue, CloseWakesBlockedConsumer) {
    BoundedQueue<int> q(1);
    std::atomic<bool> woke{false};
    std::thread consumer([&] {
        EXPECT_FALSE(q.pop().has_value());
        woke = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    q.close();
    consumer.join();
    EXPECT_TRUE(woke);
}

TEST(BoundedQueue, BlockingPushAppliesBackPressure) {
    BoundedQueue<int> q(1);
    q.push(1);
    std::atomic<bool> pushed{false};
    std::thread producer([&] {
        q.push(2);  // blocks until the consumer makes room
        pushed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(pushed);
    EXPECT_EQ(q.pop().value(), 1);
    producer.join();
    EXPECT_TRUE(pushed);
    EXPECT_EQ(q.pop().value(), 2);
}

// Many producers and consumers through a small queue: every item must arrive
// exactly once (run under ThreadSanitizer in CI).
TEST(BoundedQueue, ConcurrentProducersConsumersLoseNothing) {
    constexpr int kProducers = 4, kConsumers = 4, kPerProducer = 20000;
    BoundedQueue<int> q(64);
    std::atomic<long long> sum{0};
    std::atomic<int> count{0};

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            std::vector<int> batch;
            while (q.pop_batch(batch, 16) > 0) {
                for (int v : batch) {
                    sum += v;
                    ++count;
                }
            }
        });
    }
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) q.push(p * kPerProducer + i);
        });
    }
    for (auto& t : producers) t.join();
    q.close();
    for (auto& t : consumers) t.join();

    const long long n = static_cast<long long>(kProducers) * kPerProducer;
    EXPECT_EQ(count.load(), n);
    EXPECT_EQ(sum.load(), n * (n - 1) / 2);
    EXPECT_LE(q.high_watermark(), 64u);
}
