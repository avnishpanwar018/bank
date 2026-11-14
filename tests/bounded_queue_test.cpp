#include "bank/concurrency/bounded_queue.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <latch>
#include <thread>
#include <vector>

namespace bank {

TEST(BoundedQueueTest, ProducerAndConsumerReceiveEveryValue) {
    BoundedQueue<int> queue(4);
    std::vector<int> received;
    std::thread consumer([&] {
        while (const auto value = queue.pop()) {
            received.push_back(*value);
        }
    });

    for (int value = 0; value < 100; ++value) {
        ASSERT_TRUE(queue.push(value));
    }
    queue.close();
    consumer.join();

    ASSERT_EQ(received.size(), 100U);
    std::sort(received.begin(), received.end());
    for (int value = 0; value < 100; ++value) {
        EXPECT_EQ(received[static_cast<std::size_t>(value)], value);
    }
}

TEST(BoundedQueueTest, CloseUnblocksAllWaitingConsumers) {
    BoundedQueue<int> queue(1);
    constexpr int kConsumers = 16;
    std::latch ready(kConsumers);
    std::atomic<int> closed_results{0};
    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (int index = 0; index < kConsumers; ++index) {
        consumers.emplace_back([&] {
            ready.count_down();
            if (!queue.pop()) {
                closed_results.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    ready.wait();
    queue.close();
    for (auto& consumer : consumers) {
        consumer.join();
    }
    EXPECT_EQ(closed_results.load(std::memory_order_relaxed), kConsumers);
}

TEST(BoundedQueueTest, S3ManyProducersWithCapacityOneDoNotLoseValues) {
    BoundedQueue<int> queue(1);
    constexpr int kValues = 100;
    std::vector<int> received;
    std::atomic<int> rejected{0};
    std::thread consumer([&] {
        while (const auto value = queue.pop()) {
            received.push_back(*value);
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(kValues);
    for (int value = 0; value < kValues; ++value) {
        producers.emplace_back([&, value] {
            if (!queue.push(value)) {
                rejected.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    queue.close();
    consumer.join();

    EXPECT_EQ(rejected.load(std::memory_order_relaxed), 0);
    ASSERT_EQ(received.size(), static_cast<std::size_t>(kValues));
    std::sort(received.begin(), received.end());
    for (int value = 0; value < kValues; ++value) {
        EXPECT_EQ(received[static_cast<std::size_t>(value)], value);
    }
}

}  // namespace bank
