#include "bank/concurrency/counters.hpp"
#include "bank/concurrency/thread_pool.hpp"
#include "bank/core/account_manager.hpp"
#include "bank/core/lock_manager.hpp"
#include "bank/db/connection_pool.hpp"
#include "bank/logging/txn_log.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bank {
namespace {

std::optional<DbConfig> synchronization_config() {
    try {
        DbConfig config = DbConfig::from_environment();
        config.database = "bank_test";
        return config;
    } catch (const std::invalid_argument&) {
        if (std::getenv("BANK_DB_USER") == nullptr) {
            return std::nullopt;
        }
        throw;
    }
}

void execute(DbConnection& connection, const char* sql) {
    auto statement = connection.prepare(sql);
    statement.execute();
}

void set_account_balance(DbConnection& connection, int account_id, const std::string& amount) {
    auto statement = connection.prepare(
        "UPDATE accounts SET balance = CAST(? AS DECIMAL(15,2)), version = 0 "
        "WHERE account_id = ?");
    unsigned long amount_length = static_cast<unsigned long>(amount.size());
    MYSQL_BIND amount_param{};
    amount_param.buffer_type = MYSQL_TYPE_STRING;
    amount_param.buffer = const_cast<char*>(amount.data());
    amount_param.buffer_length = amount_length;
    amount_param.length = &amount_length;
    MYSQL_BIND id_param{};
    id_param.buffer_type = MYSQL_TYPE_LONG;
    id_param.buffer = &account_id;
    id_param.buffer_length = sizeof(account_id);
    std::array<MYSQL_BIND, 2> params{amount_param, id_param};
    statement.bind_params(params);
    statement.execute();
}

void reset_accounts(const DbConfig& config,
                    std::initializer_list<std::pair<int, std::string>> balances) {
    DbConnection connection(config);
    execute(connection, "DELETE FROM transaction_log");
    execute(connection, "DELETE FROM transactions");
    for (const auto& [account_id, amount] : balances) {
        set_account_balance(connection, account_id, amount);
    }
}

Result read_balance(const DbConfig& config, int account_id) {
    DbConnection connection(config);
    AccountManager manager(connection);
    return manager.balance(account_id);
}

Request make_request(std::string request_id, TxType type, int from, int to, Money amount) {
    Request request;
    request.request_id = std::move(request_id);
    request.type = type;
    request.from = from;
    request.to = to;
    request.amount = amount;
    return request;
}

TEST(LockManagerTest, DuplicateIdsProduceOneReentrantSafeScope) {
    LockManager manager;
    const auto locks = manager.lock({7, 7, 3, 3});
    static_cast<void>(locks);
}

TEST(SynchronizationTest, R1SafeConcurrentDepositsReachExpectedBalance) {
    const auto configured = synchronization_config();
    if (!configured) {
        GTEST_SKIP() << "BANK_DB_USER is not configured";
    }
    const DbConfig config = *configured;
    reset_accounts(config, {{1, "100.00"}});

    constexpr int kDeposits = 100;
    std::vector<std::future<Result>> results;
    {
        ConnectionPool connections(config, 16);
        BoundedQueue<Request> queue(128);
        LockManager locks;
        TxnLog log(config);
        TransactionManager transactions(connections, locks, log, false);
        OperationCounters counters;
        ThreadPool pool(16, queue, transactions, counters);
        results.reserve(kDeposits);
        for (int index = 0; index < kDeposits; ++index) {
            results.push_back(pool.submit(make_request(
                "r1-" + std::to_string(index), TxType::Deposit, 1, 0,
                Money::from_minor_units(1))));
        }
        pool.shutdown();
        for (auto& result : results) {
            ASSERT_EQ(result.get().status, TxStatus::Committed);
        }
        EXPECT_EQ(counters.committed.load(std::memory_order_relaxed),
                  static_cast<std::uint64_t>(kDeposits));
    }

    const Result balance = read_balance(config, 1);
    ASSERT_EQ(balance.status, TxStatus::Committed);
    EXPECT_EQ(balance.balance, Money::from_decimal("101.00"));
}

TEST(SynchronizationTest, R3ConcurrentWithdrawalsNeverOverdraw) {
    const auto configured = synchronization_config();
    if (!configured) {
        GTEST_SKIP() << "BANK_DB_USER is not configured";
    }
    const DbConfig config = *configured;
    reset_accounts(config, {{1, "100.00"}});

    constexpr int kWithdrawals = 50;
    int committed = 0;
    {
        ConnectionPool connections(config, 16);
        BoundedQueue<Request> queue(64);
        LockManager locks;
        TxnLog log(config);
        TransactionManager transactions(connections, locks, log, false);
        OperationCounters counters;
        ThreadPool pool(16, queue, transactions, counters);
        std::vector<std::future<Result>> results;
        results.reserve(kWithdrawals);
        for (int index = 0; index < kWithdrawals; ++index) {
            results.push_back(pool.submit(make_request(
                "r3-" + std::to_string(index), TxType::Withdraw, 1, 0,
                Money::from_decimal("10.00"))));
        }
        pool.shutdown();
        for (auto& result : results) {
            if (result.get().status == TxStatus::Committed) {
                ++committed;
            }
        }
        EXPECT_EQ(counters.total.load(std::memory_order_relaxed),
                  static_cast<std::uint64_t>(kWithdrawals));
    }

    const Result balance = read_balance(config, 1);
    ASSERT_EQ(balance.status, TxStatus::Committed);
    EXPECT_EQ(committed, 10);
    EXPECT_EQ(balance.balance, Money::from_minor_units(0));
}

TEST(SynchronizationTest, D1OpposingTransfersCompleteWithoutDeadlock) {
    const auto configured = synchronization_config();
    if (!configured) {
        GTEST_SKIP() << "BANK_DB_USER is not configured";
    }
    const DbConfig config = *configured;
    reset_accounts(config, {{1, "100.00"}, {2, "100.00"}});

    constexpr int kTransfersPerDirection = 10000;
    std::vector<std::future<Result>> results;
    int committed = 0;
    int rolled_back = 0;
    {
        ConnectionPool connections(config, 16);
        BoundedQueue<Request> queue(256);
        LockManager locks;
        TxnLog log(config);
        TransactionManager transactions(connections, locks, log, false);
        OperationCounters counters;
        ThreadPool pool(16, queue, transactions, counters);
        results.reserve(kTransfersPerDirection * 2);
        for (int index = 0; index < kTransfersPerDirection; ++index) {
            results.push_back(pool.submit(make_request(
                "d1-ab-" + std::to_string(index), TxType::Transfer, 1, 2,
                Money::from_minor_units(1))));
            results.push_back(pool.submit(make_request(
                "d1-ba-" + std::to_string(index), TxType::Transfer, 2, 1,
                Money::from_minor_units(1))));
        }
        pool.shutdown();
        for (auto& result : results) {
            const Result completed = result.get();
            if (completed.status == TxStatus::Committed) {
                ++committed;
            } else {
                ++rolled_back;
            }
        }
        EXPECT_EQ(counters.total.load(std::memory_order_relaxed),
                  static_cast<std::uint64_t>(kTransfersPerDirection * 2));
    }

    std::cout << "D1 committed=" << committed << " rolled_back=" << rolled_back << '\n';
    EXPECT_EQ(committed + rolled_back, kTransfersPerDirection * 2);

    const Result first = read_balance(config, 1);
    const Result second = read_balance(config, 2);
    ASSERT_EQ(first.status, TxStatus::Committed);
    ASSERT_EQ(second.status, TxStatus::Committed);
    EXPECT_GE(first.balance.minor_units(), 0);
    EXPECT_GE(second.balance.minor_units(), 0);
    EXPECT_EQ(first.balance.minor_units() + second.balance.minor_units(),
              Money::from_decimal("200.00").minor_units());
}

TEST(SynchronizationTest, D2RingTransfersCompleteWithoutDeadlock) {
    const auto configured = synchronization_config();
    if (!configured) {
        GTEST_SKIP() << "BANK_DB_USER is not configured";
    }
    const DbConfig config = *configured;
    reset_accounts(config, {{1, "100.00"}, {2, "100.00"}, {3, "100.00"}});

    constexpr int kTransfersPerEdge = 1000;
    std::vector<std::future<Result>> results;
    {
        ConnectionPool connections(config, 16);
        BoundedQueue<Request> queue(128);
        LockManager locks;
        TxnLog log(config);
        TransactionManager transactions(connections, locks, log, false);
        OperationCounters counters;
        ThreadPool pool(16, queue, transactions, counters);
        results.reserve(kTransfersPerEdge * 3);
        for (int index = 0; index < kTransfersPerEdge; ++index) {
            results.push_back(pool.submit(make_request(
                "d2-ab-" + std::to_string(index), TxType::Transfer, 1, 2,
                Money::from_minor_units(1))));
            results.push_back(pool.submit(make_request(
                "d2-bc-" + std::to_string(index), TxType::Transfer, 2, 3,
                Money::from_minor_units(1))));
            results.push_back(pool.submit(make_request(
                "d2-ca-" + std::to_string(index), TxType::Transfer, 3, 1,
                Money::from_minor_units(1))));
        }
        pool.shutdown();
        for (auto& result : results) {
            ASSERT_EQ(result.get().status, TxStatus::Committed);
        }
        EXPECT_EQ(counters.committed.load(std::memory_order_relaxed),
                  static_cast<std::uint64_t>(kTransfersPerEdge * 3));
    }

    std::int64_t total = 0;
    for (int account_id = 1; account_id <= 3; ++account_id) {
        const Result balance = read_balance(config, account_id);
        ASSERT_EQ(balance.status, TxStatus::Committed);
        EXPECT_GE(balance.balance.minor_units(), 0);
        total += balance.balance.minor_units();
    }
    EXPECT_EQ(total, Money::from_decimal("300.00").minor_units());
}

}  // namespace
}  // namespace bank
