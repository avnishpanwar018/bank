#include "bank/concurrency/bounded_queue.hpp"
#include "bank/concurrency/counters.hpp"
#include "bank/sim/monitor.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace bank {
namespace {

TEST(MonitorTest, EmitsSnapshotAndStopsWithoutWaitingForAnInterval) {
    BoundedQueue<Request> queue(8);
    OperationCounters counters;
    counters.committed.store(3, std::memory_order_relaxed);
    counters.rolled_back.store(1, std::memory_order_relaxed);
    counters.total.store(4, std::memory_order_relaxed);
    counters.active.store(2, std::memory_order_relaxed);
    counters.lock_waits.store(5, std::memory_order_relaxed);
    counters.service_latency_ns.store(8'000'000, std::memory_order_relaxed);
    std::ostringstream output;

    Monitor monitor(queue, counters, output, std::chrono::hours{1});
    monitor.start();
    monitor.stop();
    monitor.stop();

    const std::string text = output.str();
    EXPECT_NE(text.find("queue=0/8"), std::string::npos);
    EXPECT_NE(text.find("active=2"), std::string::npos);
    EXPECT_NE(text.find("committed=3"), std::string::npos);
    EXPECT_NE(text.find("rolled_back=1"), std::string::npos);
    EXPECT_NE(text.find("throughput="), std::string::npos);
    EXPECT_NE(text.find("avg_service_latency=2.00ms"), std::string::npos);
    EXPECT_NE(text.find("lock_waits_total=5"), std::string::npos);
}

}  // namespace
}  // namespace bank
