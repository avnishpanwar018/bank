#include "bank/concurrency/counters.hpp"
#include "bank/concurrency/thread_pool.hpp"
#include "bank/core/transaction_manager.hpp"
#include "bank/db/connection_pool.hpp"
#include "bank/logging/txn_log.hpp"
#include "bank/tools/consistency_checker.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace bank {
namespace {

std::optional<DbConfig> stress_config() {
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

void set_balance(DbConnection& connection, int account_id, const std::string& amount) {
    auto statement = connection.prepare(
        "UPDATE accounts SET balance = CAST(? AS DECIMAL(15,2)), version = 0 "
        "WHERE account_id = ?");
    unsigned long amount_length = static_cast<unsigned long>(amount.size());
    MYSQL_BIND amount_bind{};
    amount_bind.buffer_type = MYSQL_TYPE_STRING;
    amount_bind.buffer = const_cast<char*>(amount.data());
    amount_bind.buffer_length = amount_length;
    amount_bind.length = &amount_length;
    int account_id_copy = account_id;
    MYSQL_BIND id_bind{};
    id_bind.buffer_type = MYSQL_TYPE_LONG;
    id_bind.buffer = &account_id_copy;
    id_bind.buffer_length = sizeof(account_id_copy);
    std::array<MYSQL_BIND, 2> params{amount_bind, id_bind};
    statement.bind_params(params);
    statement.execute();
}

std::map<int, Money> reset_stress_database(const DbConfig& config) {
    DbConnection connection(config);
    execute(connection, "DELETE FROM transaction_log");
    execute(connection, "DELETE FROM transactions");
    std::map<int, Money> baseline;
    for (int account_id = 1; account_id <= 50; ++account_id) {
        set_balance(connection, account_id, "100000.00");
        baseline.emplace(account_id, Money::from_decimal("100000.00"));
    }
    return baseline;
}

Request make_request(std::size_t client, std::size_t operation, std::size_t account_count,
                     double hotspot_ratio, std::uint64_t seed) {
    std::mt19937_64 generator(seed + client * 991ULL + operation * 31337ULL);
    std::uniform_int_distribution<int> operation_distribution(0, 3);
    std::uniform_int_distribution<std::int64_t> amount_distribution(1, 10000);
    std::uniform_int_distribution<int> account_distribution(1, static_cast<int>(account_count));
    std::uniform_real_distribution<double> ratio_distribution(0.0, 1.0);
    const auto choose_account = [&] {
        if (ratio_distribution(generator) < hotspot_ratio) {
            return 1 + static_cast<int>(generator() % 2ULL);
        }
        return account_distribution(generator);
    };

    Request request;
    request.request_id = "stress-" + std::to_string(client) + "-" + std::to_string(operation);
    request.from = choose_account();
    request.to = choose_account();
    request.amount = Money::from_minor_units(amount_distribution(generator));
    switch (operation_distribution(generator)) {
    case 0:
        request.type = TxType::Deposit;
        break;
    case 1:
        request.type = TxType::Withdraw;
        break;
    case 2:
        request.type = TxType::Transfer;
        break;
    default:
        request.type = TxType::Balance;
        request.amount = Money{};
        break;
    }
    return request;
}

void run_stress(const DbConfig& config, std::size_t workers, std::size_t pool_size,
                double hotspot_ratio, std::uint64_t seed, const char* label) {
    const std::map<int, Money> baseline = reset_stress_database(config);
    BoundedQueue<Request> queue(512);
    ConnectionPool connections(config, pool_size);
    LockManager locks;
    TxnLog log(config);
    TransactionManager transactions(connections, locks, log, false);
    OperationCounters counters;
    ThreadPool pool(workers, queue, transactions, counters);
    constexpr std::size_t kClients = 100;
    constexpr std::size_t kOperations = 100;
    std::vector<std::thread> clients;
    clients.reserve(kClients);
    for (std::size_t client = 0; client < kClients; ++client) {
        clients.emplace_back([&, client] {
            std::vector<std::future<Result>> results;
            results.reserve(kOperations);
            for (std::size_t operation = 0; operation < kOperations; ++operation) {
                results.push_back(pool.submit(make_request(client, operation, 50,
                                                           hotspot_ratio, seed)));
            }
            for (auto& result : results) {
                static_cast<void>(result.get());
            }
        });
    }
    for (auto& client : clients) {
        client.join();
    }
    pool.shutdown();

    DbConnection checker_connection(config);
    const ConsistencyReport report = ConsistencyChecker(checker_connection).check(baseline);
    std::cout << label << " seed=" << seed
              << " committed=" << counters.committed.load(std::memory_order_relaxed)
              << " rolled_back=" << counters.rolled_back.load(std::memory_order_relaxed)
              << " I1=" << report.i1_total_conserved
              << " I2=" << report.i2_no_negative_balance
              << " I3=" << report.i3_final_status
              << " I4=" << report.i4_no_pending
              << " I5=" << report.i5_ledger_matches << '\n';
    EXPECT_EQ(counters.total.load(std::memory_order_relaxed),
              static_cast<std::uint64_t>(kClients * kOperations));
    EXPECT_TRUE(report.passed);
}

TEST(StressTest, S1HundredClientsMixedOperations) {
    const auto config = stress_config();
    if (!config) {
        GTEST_SKIP() << "BANK_DB_USER is not configured";
    }
    const char* seed_text = std::getenv("BANK_STRESS_SEED");
    const std::uint64_t seed = seed_text == nullptr ? 1 : std::strtoull(seed_text, nullptr, 10);
    run_stress(*config, 16, 16, 0.0, seed, "S1");
}

TEST(StressTest, S2NinetyPercentHotspot) {
    const auto config = stress_config();
    if (!config) {
        GTEST_SKIP() << "BANK_DB_USER is not configured";
    }
    run_stress(*config, 16, 16, 0.90, 2, "S2");
}

TEST(StressTest, S4PoolSizeTwoWithThirtyTwoWorkers) {
    const auto config = stress_config();
    if (!config) {
        GTEST_SKIP() << "BANK_DB_USER is not configured";
    }
    run_stress(*config, 32, 2, 0.0, 4, "S4");
}

TEST(StressTest, S3RequestQueueSizeOneWithHundredProducers) {
    const auto config = stress_config();
    if (!config) {
        GTEST_SKIP() << "BANK_DB_USER is not configured";
    }
    const std::map<int, Money> baseline = reset_stress_database(*config);
    BoundedQueue<Request> queue(1);
    ConnectionPool connections(*config, 16);
    LockManager locks;
    TxnLog log(*config);
    TransactionManager transactions(connections, locks, log, false);
    OperationCounters counters;
    ThreadPool pool(16, queue, transactions, counters);
    std::vector<std::thread> producers;
    producers.reserve(100);
    for (int index = 0; index < 100; ++index) {
        producers.emplace_back([&, index] {
            Request value;
            value.request_id = "s3-request-" + std::to_string(index);
            value.type = TxType::Deposit;
            value.from = 1;
            value.amount = Money::from_minor_units(1);
            const Result result = pool.submit(std::move(value)).get();
            EXPECT_EQ(result.status, TxStatus::Committed);
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    pool.shutdown();
    DbConnection checker_connection(*config);
    const ConsistencyReport report = ConsistencyChecker(checker_connection).check(baseline);
    EXPECT_EQ(counters.total.load(std::memory_order_relaxed), 100U);
    EXPECT_TRUE(report.passed);
}

}  // namespace
}  // namespace bank
