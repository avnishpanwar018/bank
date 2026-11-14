#include "bank/concurrency/counters.hpp"
#include "bank/concurrency/thread_pool.hpp"
#include "bank/core/account_manager.hpp"
#include "bank/core/lock_manager.hpp"
#include "bank/db/connection_pool.hpp"
#include "bank/logging/txn_log.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <optional>
#include <thread>
#include <vector>

namespace bank {
namespace {

std::optional<DbConfig> test_config() {
    try {
        return DbConfig::from_environment();
    } catch (const std::invalid_argument& error) {
        if (std::getenv("BANK_DB_USER") == nullptr) {
            return std::nullopt;
        }
        throw;
    }
}

TEST(ConnectionPoolTest, SixteenThreadsNeverHoldMoreThanTwoLeases) {
    const auto configured = test_config();
    if (!configured) {
        GTEST_SKIP() << "BANK_DB_USER is not configured";
    }
    DbConfig config = *configured;
    config.database = "bank_test";
    ConnectionPool pool(config, 2);
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    std::vector<std::thread> workers;
    workers.reserve(16);
    for (int worker = 0; worker < 16; ++worker) {
        workers.emplace_back([&] {
            for (int iteration = 0; iteration < 20; ++iteration) {
                auto lease = pool.acquire();
                const int now_active = active.fetch_add(1, std::memory_order_relaxed) + 1;
                int observed = maximum.load(std::memory_order_relaxed);
                while (now_active > observed &&
                       !maximum.compare_exchange_weak(observed, now_active,
                                                      std::memory_order_relaxed)) {
                }
                active.fetch_sub(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(active.load(std::memory_order_relaxed), 0);
    EXPECT_LE(maximum.load(std::memory_order_relaxed), 2);
}

TEST(ThreadPoolTest, AllSubmittedRequestsCompleteExactlyOnce) {
    const auto configured = test_config();
    if (!configured) {
        GTEST_SKIP() << "BANK_DB_USER is not configured";
    }
    DbConfig config = *configured;
    config.database = "bank_test";
    ConnectionPool connections(config, 2);
    BoundedQueue<Request> queue(4);
    LockManager lock_manager;
    TxnLog log(config);
    TransactionManager transactions(connections, lock_manager, log, false);
    OperationCounters counters;
    ThreadPool pool(4, queue, transactions, counters);

    constexpr int kRequests = 32;
    std::vector<std::future<Result>> results;
    results.reserve(kRequests);
    for (int index = 0; index < kRequests; ++index) {
        Request request;
        request.request_id = "thread-pool-balance-" + std::to_string(index);
        request.type = TxType::Balance;
        request.from = 1;
        results.push_back(pool.submit(std::move(request)));
    }
    pool.shutdown();

    for (auto& result : results) {
        ASSERT_TRUE(result.valid());
        const Result completed = result.get();
        EXPECT_EQ(completed.status, TxStatus::Committed);
    }
    EXPECT_EQ(counters.total.load(std::memory_order_relaxed), kRequests);
    EXPECT_EQ(counters.committed.load(std::memory_order_relaxed), kRequests);
    EXPECT_EQ(counters.rolled_back.load(std::memory_order_relaxed), 0);
}

void reset_r2_account(const DbConfig& config) {
    DbConnection connection(config);
    for (const char* sql : {"DELETE FROM transaction_log", "DELETE FROM transactions"}) {
        auto statement = connection.prepare(sql);
        statement.execute();
    }
    auto statement = connection.prepare(
        "UPDATE accounts SET balance = CAST(? AS DECIMAL(15,2)), version = 0 "
        "WHERE account_id = ?");
    std::string amount = "100.00";
    unsigned long amount_length = static_cast<unsigned long>(amount.size());
    int account_id = 1;
    MYSQL_BIND amount_param{};
    amount_param.buffer_type = MYSQL_TYPE_STRING;
    amount_param.buffer = amount.data();
    amount_param.buffer_length = amount_length;
    amount_param.length = &amount_length;
    MYSQL_BIND account_param{};
    account_param.buffer_type = MYSQL_TYPE_LONG;
    account_param.buffer = &account_id;
    account_param.buffer_length = sizeof(account_id);
    std::array<MYSQL_BIND, 2> params{amount_param, account_param};
    statement.bind_params(params);
    statement.execute();
}

TEST(UnsafeRaceTest, R2ReportsLostUpdatesAcrossTwentyRuns) {
    const auto configured = test_config();
    if (!configured) {
        GTEST_SKIP() << "BANK_DB_USER is not configured";
    }
    DbConfig config = *configured;
    config.database = "bank_test";

    constexpr int kRuns = 20;
    constexpr int kDeposits = 100;
    int lost_update_runs = 0;
    std::cout << "R2 run | expected cents | actual cents | lost cents\n";
    for (int run = 0; run < kRuns; ++run) {
        reset_r2_account(config);
        ConnectionPool connections(config, 16);
        BoundedQueue<Request> queue(128);
        LockManager lock_manager;
        TxnLog log(config);
        TransactionManager transactions(connections, lock_manager, log, true);
        OperationCounters counters;
        ThreadPool pool(16, queue, transactions, counters);
        std::vector<std::future<Result>> results;
        results.reserve(kDeposits);
        for (int deposit = 0; deposit < kDeposits; ++deposit) {
            Request request;
            request.request_id = "r2-" + std::to_string(run) + "-" + std::to_string(deposit);
            request.type = TxType::Deposit;
            request.from = 1;
            request.amount = Money::from_minor_units(1);
            results.push_back(pool.submit(std::move(request)));
        }
        pool.shutdown();
        for (auto& result : results) {
            ASSERT_EQ(result.get().status, TxStatus::Committed);
        }
        ASSERT_EQ(counters.total.load(std::memory_order_relaxed),
                  static_cast<std::uint64_t>(kDeposits));

        DbConnection read_connection(config);
        AccountManager manager(read_connection);
        const Result balance = manager.balance(1);
        ASSERT_EQ(balance.status, TxStatus::Committed);
        const std::int64_t expected = 10000 + kDeposits;
        const std::int64_t actual = balance.balance.minor_units();
        const std::int64_t loss = expected - actual;
        std::cout << run + 1 << " | " << expected << " | " << actual << " | " << loss << '\n';
        EXPECT_LE(actual, expected);
        if (actual < expected) {
            ++lost_update_runs;
        }
    }
    std::cout << "R2 lost-update runs: " << lost_update_runs << '/' << kRuns << '\n';
    EXPECT_GT(lost_update_runs, 0);
}

}  // namespace
}  // namespace bank
