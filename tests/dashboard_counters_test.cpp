#include "bank/concurrency/counters.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <latch>
#include <limits>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bank {
namespace {

class LatchRelease final {
public:
    explicit LatchRelease(std::latch& latch) : latch_(latch) {}
    ~LatchRelease() { release(); }

    void release() noexcept {
        if (!released_) {
            latch_.count_down();
            released_ = true;
        }
    }

private:
    std::latch& latch_;
    bool released_ = false;
};

TEST(DashboardCountersTest, StartsEmptyAndRecordsFinalStatusRetryAndLatencyTotals) {
    OperationCounters counters;
    const auto empty = counters.snapshot();
    EXPECT_EQ(empty.total, 0U);
    EXPECT_EQ(empty.committed, 0U);
    EXPECT_EQ(empty.rolled_back, 0U);
    EXPECT_EQ(empty.active, 0U);
    EXPECT_EQ(empty.lock_waits, 0U);
    EXPECT_EQ(empty.retries, 0U);
    EXPECT_EQ(empty.service_latency_ns, 0U);
    EXPECT_EQ(std::accumulate(empty.latency_buckets.begin(), empty.latency_buckets.end(),
                              std::uint64_t{0}), 0U);
    EXPECT_EQ(std::accumulate(empty.rolled_back_by_reason.begin(), empty.rolled_back_by_reason.end(),
                              std::uint64_t{0}), 0U);

    counters.record_completion({TxStatus::Committed, "", Money{}, 3}, 10);
    counters.record_completion({TxStatus::RolledBack, "INSUFFICIENT_FUNDS", Money{}, 2}, 20);
    counters.record_completion({TxStatus::RolledBack, "unrecognized failure", Money{}, 1}, 30);
    counters.active.store(2, std::memory_order_relaxed);
    counters.lock_waits.store(7, std::memory_order_relaxed);
    const auto snapshot = counters.snapshot();
    EXPECT_EQ(snapshot.total, 3U);
    EXPECT_EQ(snapshot.committed, 1U);
    EXPECT_EQ(snapshot.rolled_back, 2U);
    EXPECT_EQ(snapshot.retries, 3U);
    EXPECT_EQ(snapshot.active, 2U);
    EXPECT_EQ(snapshot.lock_waits, 7U);
    EXPECT_EQ(snapshot.service_latency_ns, 60U);
    EXPECT_EQ(snapshot.rolled_back_by_reason[static_cast<std::size_t>(RollbackReason::InsufficientFunds)], 1U);
    EXPECT_EQ(snapshot.rolled_back_by_reason[static_cast<std::size_t>(RollbackReason::Other)], 1U);
    EXPECT_EQ(std::accumulate(snapshot.latency_buckets.begin(), snapshot.latency_buckets.end(),
                              std::uint64_t{0}), snapshot.total);
}

TEST(DashboardCountersTest, ClassifiesKnownReasonPrefixesAndPreservesUnknownFailures) {
    const std::array<std::pair<const char*, RollbackReason>, kRollbackReasonCount> reasons{{
        {"INSUFFICIENT_FUNDS: account 1", RollbackReason::InsufficientFunds},
        {"ACCOUNT_NOT_FOUND: 99", RollbackReason::AccountNotFound},
        {"ACCOUNT_NOT_ACTIVE: frozen", RollbackReason::AccountNotActive},
        {"INVALID_AMOUNT: zero", RollbackReason::InvalidAmount},
        {"SAME_ACCOUNT: 1", RollbackReason::SameAccount},
        {"DB_DEADLOCK_EXHAUSTED: retries", RollbackReason::DeadlockExhausted},
        {"DB_ERROR: unavailable", RollbackReason::DatabaseError},
        {"QUEUE_CLOSED: shutdown", RollbackReason::QueueClosed},
        {"WORKER_ERROR: exception", RollbackReason::WorkerError},
        {"future-new-error", RollbackReason::Other}}};
    OperationCounters counters;
    for (const auto& [reason, category] : reasons) {
        SCOPED_TRACE(reason);
        counters.record_completion({TxStatus::RolledBack, reason, Money{}, 1}, 1);
        EXPECT_EQ(counters.snapshot().rolled_back_by_reason[static_cast<std::size_t>(category)], 1U);
    }
    counters.record_completion({TxStatus::RolledBack, "", Money{}, 0}, 0);
    const auto snapshot = counters.snapshot();
    EXPECT_EQ(snapshot.total, reasons.size() + 1);
    EXPECT_EQ(snapshot.committed, 0U);
    EXPECT_EQ(snapshot.rolled_back, snapshot.total);
    EXPECT_EQ(snapshot.retries, 0U);
    EXPECT_EQ(snapshot.rolled_back_by_reason[static_cast<std::size_t>(RollbackReason::Other)], 2U);
    EXPECT_EQ(std::accumulate(snapshot.rolled_back_by_reason.begin(), snapshot.rolled_back_by_reason.end(),
                              std::uint64_t{0}), snapshot.rolled_back);
}

TEST(DashboardCountersTest, HistogramHandlesZeroPowerOfTwoBoundariesAndSaturatingTail) {
    OperationCounters counters;
    constexpr std::array<std::uint64_t, 8> latencies{
        0, 1, 2, 3, 4, 7, 8, std::numeric_limits<std::uint64_t>::max() - 25};
    for (const auto latency : latencies) {
        counters.record_completion({TxStatus::Committed, "", Money{}, 1}, latency);
    }
    const auto snapshot = counters.snapshot();
    EXPECT_EQ(snapshot.total, latencies.size());
    EXPECT_EQ(snapshot.latency_buckets[0], 2U);
    EXPECT_EQ(snapshot.latency_buckets[1], 2U);
    EXPECT_EQ(snapshot.latency_buckets[2], 2U);
    EXPECT_EQ(snapshot.latency_buckets[3], 1U);
    EXPECT_EQ(snapshot.latency_buckets.back(), 1U);
    EXPECT_EQ(std::accumulate(snapshot.latency_buckets.begin(), snapshot.latency_buckets.end(),
                              std::uint64_t{0}), snapshot.total);
    EXPECT_EQ(snapshot.service_latency_ns, std::numeric_limits<std::uint64_t>::max());
}

TEST(DashboardCountersTest, ConcurrentCompletionsAndLiveSnapshotsDoNotLoseUpdates) {
    constexpr std::size_t kWriters = 16;
    constexpr std::size_t kOperations = 256;
    OperationCounters counters;
    std::latch start(1);
    std::latch midpoint(kWriters);
    std::latch resume(1);
    std::latch finished(kWriters);
    std::vector<std::jthread> writers;
    LatchRelease release_start(start);
    LatchRelease release_resume(resume);
    for (std::size_t writer = 0; writer < kWriters; ++writer) {
        writers.emplace_back([&, writer] {
            start.wait();
            const Result result = writer % 2 == 0
                ? Result{TxStatus::Committed, "", Money{}, 3}
                : Result{TxStatus::RolledBack, "INSUFFICIENT_FUNDS", Money{}, 1};
            for (std::size_t operation = 0; operation < kOperations; ++operation) {
                counters.record_completion(result, 8);
                if (operation + 1 == kOperations / 2) {
                    midpoint.count_down();
                    resume.wait();
                }
            }
            finished.count_down();
        });
    }
    release_start.release();
    // Relaxed multi-atomic snapshots need not be mutually coherent in flight.
    const auto live = counters.snapshot();
    EXPECT_LE(live.total, kWriters * kOperations / 2);
    midpoint.wait();
    const auto half = counters.snapshot();
    EXPECT_GE(half.total, live.total);
    EXPECT_EQ(half.total, kWriters * kOperations / 2);
    EXPECT_EQ(half.committed, kWriters * kOperations / 4);
    EXPECT_EQ(half.rolled_back, kWriters * kOperations / 4);
    release_resume.release();
    finished.wait();
    writers.clear();
    const auto snapshot = counters.snapshot();
    EXPECT_EQ(snapshot.total, kWriters * kOperations);
    EXPECT_EQ(snapshot.committed, kWriters * kOperations / 2);
    EXPECT_EQ(snapshot.rolled_back, kWriters * kOperations / 2);
    EXPECT_EQ(snapshot.retries, kWriters * kOperations);
    EXPECT_EQ(snapshot.service_latency_ns, kWriters * kOperations * 8);
    EXPECT_EQ(snapshot.latency_buckets[3], snapshot.total);
    EXPECT_EQ(snapshot.rolled_back_by_reason[static_cast<std::size_t>(RollbackReason::InsufficientFunds)],
              snapshot.rolled_back);
}

}  // namespace
}  // namespace bank
