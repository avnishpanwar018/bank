#pragma once

#include "bank/types.hpp"

#include <atomic>
#include <array>
#include <bit>
#include <cstdint>
#include <string_view>

namespace bank {

enum class RollbackReason : std::size_t {
    InsufficientFunds = 0,
    AccountNotFound,
    AccountNotActive,
    InvalidAmount,
    SameAccount,
    DeadlockExhausted,
    DatabaseError,
    QueueClosed,
    WorkerError,
    Other,
    Count,
};

inline constexpr std::size_t kRollbackReasonCount =
    static_cast<std::size_t>(RollbackReason::Count);
inline constexpr std::size_t kLatencyBucketCount = 64;

struct CounterSnapshot {
    std::uint64_t committed = 0;
    std::uint64_t rolled_back = 0;
    std::uint64_t total = 0;
    std::uint64_t active = 0;
    std::uint64_t lock_waits = 0;
    std::uint64_t service_latency_ns = 0;
    std::uint64_t retries = 0;
    std::array<std::uint64_t, kRollbackReasonCount> rolled_back_by_reason{};
    std::array<std::uint64_t, kLatencyBucketCount> latency_buckets{};
};

struct OperationCounters {
    std::atomic<std::uint64_t> committed{0};
    std::atomic<std::uint64_t> rolled_back{0};
    std::atomic<std::uint64_t> total{0};
    std::atomic<std::uint64_t> active{0};
    std::atomic<std::uint64_t> lock_waits{0};
    std::atomic<std::uint64_t> service_latency_ns{0};
    std::atomic<std::uint64_t> retries{0};
    std::array<std::atomic<std::uint64_t>, kRollbackReasonCount> rollback_reasons{};
    std::array<std::atomic<std::uint64_t>, kLatencyBucketCount> latency_buckets{};

    void record_completion(const Result& result, std::uint64_t latency_ns) noexcept {
        total.fetch_add(1, std::memory_order_relaxed);
        if (result.status == TxStatus::Committed) {
            committed.fetch_add(1, std::memory_order_relaxed);
        } else {
            rolled_back.fetch_add(1, std::memory_order_relaxed);
            rollback_reasons[static_cast<std::size_t>(classify_reason(result.reason))]
                .fetch_add(1, std::memory_order_relaxed);
        }
        if (result.attempts > 1) {
            retries.fetch_add(static_cast<std::uint64_t>(result.attempts - 1),
                              std::memory_order_relaxed);
        }
        service_latency_ns.fetch_add(latency_ns, std::memory_order_relaxed);
        latency_buckets[latency_bucket(latency_ns)].fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] CounterSnapshot snapshot() const noexcept {
        return {
            committed.load(std::memory_order_relaxed),
            rolled_back.load(std::memory_order_relaxed),
            total.load(std::memory_order_relaxed),
            active.load(std::memory_order_relaxed),
            lock_waits.load(std::memory_order_relaxed),
            service_latency_ns.load(std::memory_order_relaxed),
            retries.load(std::memory_order_relaxed),
            load_array(rollback_reasons),
            load_array(latency_buckets)};
    }

private:
    [[nodiscard]] static RollbackReason classify_reason(std::string_view reason) noexcept {
        if (reason.starts_with("INSUFFICIENT_FUNDS")) {
            return RollbackReason::InsufficientFunds;
        }
        if (reason.starts_with("ACCOUNT_NOT_FOUND")) {
            return RollbackReason::AccountNotFound;
        }
        if (reason.starts_with("ACCOUNT_NOT_ACTIVE")) {
            return RollbackReason::AccountNotActive;
        }
        if (reason.starts_with("INVALID_AMOUNT")) {
            return RollbackReason::InvalidAmount;
        }
        if (reason.starts_with("SAME_ACCOUNT")) {
            return RollbackReason::SameAccount;
        }
        if (reason.starts_with("DB_DEADLOCK_EXHAUSTED")) {
            return RollbackReason::DeadlockExhausted;
        }
        if (reason.starts_with("DB_ERROR")) {
            return RollbackReason::DatabaseError;
        }
        if (reason.starts_with("QUEUE_CLOSED")) {
            return RollbackReason::QueueClosed;
        }
        if (reason.starts_with("WORKER_ERROR")) {
            return RollbackReason::WorkerError;
        }
        return RollbackReason::Other;
    }

    [[nodiscard]] static std::size_t latency_bucket(std::uint64_t latency_ns) noexcept {
        return latency_ns == 0 ? 0 : static_cast<std::size_t>(std::bit_width(latency_ns) - 1);
    }

    template <std::size_t Size>
    [[nodiscard]] static std::array<std::uint64_t, Size> load_array(
        const std::array<std::atomic<std::uint64_t>, Size>& values) noexcept {
        std::array<std::uint64_t, Size> result{};
        for (std::size_t index = 0; index < Size; ++index) {
            result[index] = values[index].load(std::memory_order_relaxed);
        }
        return result;
    }
};

}  // namespace bank
