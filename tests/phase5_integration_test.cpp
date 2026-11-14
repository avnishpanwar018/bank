#include "bank/core/transaction_manager.hpp"
#include "bank/core/account_manager.hpp"
#include "bank/db/connection_pool.hpp"
#include "bank/tools/consistency_checker.hpp"
#include "bank/tools/recovery.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <map>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace bank {
namespace {

std::optional<DbConfig> phase5_config() {
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
    MYSQL_BIND id_bind{};
    id_bind.buffer_type = MYSQL_TYPE_LONG;
    id_bind.buffer = &account_id;
    id_bind.buffer_length = sizeof(account_id);
    std::array<MYSQL_BIND, 2> params{amount_bind, id_bind};
    statement.bind_params(params);
    statement.execute();
}

void reset(const DbConfig& config, std::initializer_list<std::pair<int, std::string>> balances) {
    DbConnection connection(config);
    execute(connection, "DELETE FROM transaction_log");
    execute(connection, "DELETE FROM transactions");
    // Phase 5 tests share bank_test, so restore every seeded account before
    // applying the scenario-specific balances. This keeps I5 independent of
    // earlier stress tests that intentionally mutate all 50 accounts.
    for (int account_id = 1; account_id <= 50; ++account_id) {
        set_balance(connection, account_id, "100000.00");
    }
    for (const auto& [account_id, amount] : balances) {
        set_balance(connection, account_id, amount);
    }
}

Request request(std::string id, TxType type, int from, int to, Money amount) {
    Request value;
    value.request_id = std::move(id);
    value.type = type;
    value.from = from;
    value.to = to;
    value.amount = amount;
    return value;
}

struct Phase5Fixture : ::testing::Test {
    std::optional<DbConfig> config;

    void SetUp() override {
        config = phase5_config();
        if (!config) {
            GTEST_SKIP() << "BANK_DB_USER is not configured";
        }
    }
};

TEST_F(Phase5Fixture, D3RetriesDeadlockAndSucceedsOnThirdAttempt) {
    reset(*config, {{1, "100.00"}});
    ConnectionPool connections(*config, 1);
    LockManager locks;
    TxnLog log(*config);
    int injected = 0;
    TransactionManager manager(connections, locks, log, false,
        [&](FaultPoint, DbConnection&, const Request&, int) {
            if (injected++ < 2) {
                throw DbError(1213, "forced deadlock");
            }
        });

    Request value = request("d3-success", TxType::Deposit, 1, 0, Money::from_decimal("1.00"));
    const Result result = manager.execute(value);
    EXPECT_EQ(result.status, TxStatus::Committed);
    EXPECT_EQ(result.attempts, 3);
}

TEST_F(Phase5Fixture, D3ExhaustionReturnsFinalRollback) {
    reset(*config, {{1, "100.00"}});
    ConnectionPool connections(*config, 1);
    LockManager locks;
    TxnLog log(*config);
    TransactionManager manager(connections, locks, log, false,
        [](FaultPoint, DbConnection&, const Request&, int) {
            throw DbError(1205, "forced lock timeout");
        });

    Request value = request("d3-exhausted", TxType::Deposit, 1, 0, Money::from_decimal("1.00"));
    const Result result = manager.execute(value);
    EXPECT_EQ(result.status, TxStatus::RolledBack);
    EXPECT_EQ(result.reason, "DB_DEADLOCK_EXHAUSTED");
    EXPECT_EQ(result.attempts, 3);
}

TEST_F(Phase5Fixture, X4DatabaseCheckConstraintRejectsNegativeBalance) {
    reset(*config, {{1, "100.00"}});
    DbConnection connection(*config);
    auto statement = connection.prepare(
        "UPDATE accounts SET balance = CAST(? AS DECIMAL(15,2)) WHERE account_id = ?");
    std::string amount = "-1.00";
    unsigned long amount_length = static_cast<unsigned long>(amount.size());
    int account_id = 1;
    MYSQL_BIND amount_bind{};
    amount_bind.buffer_type = MYSQL_TYPE_STRING;
    amount_bind.buffer = amount.data();
    amount_bind.buffer_length = amount_length;
    amount_bind.length = &amount_length;
    MYSQL_BIND id_bind{};
    id_bind.buffer_type = MYSQL_TYPE_LONG;
    id_bind.buffer = &account_id;
    id_bind.buffer_length = sizeof(account_id);
    std::array<MYSQL_BIND, 2> params{amount_bind, id_bind};
    statement.bind_params(params);
    try {
        statement.execute();
        FAIL() << "negative balance update unexpectedly succeeded";
    } catch (const DbError& error) {
        EXPECT_EQ(error.code(), 3819U);
    }
}

TEST_F(Phase5Fixture, X1InjectedFailureAfterDebitRollsBackBothAccounts) {
    reset(*config, {{1, "100.00"}, {2, "100.00"}});
    ConnectionPool connections(*config, 1);
    LockManager locks;
    TxnLog log(*config);
    TransactionManager manager(connections, locks, log, false,
        [](FaultPoint point, DbConnection&, const Request&, int) {
            if (point == FaultPoint::AfterDebitBeforeCredit) {
                throw std::runtime_error("injected after debit before credit");
            }
        });
    Request value = request("x1-after-debit", TxType::Transfer, 1, 2,
                            Money::from_decimal("10.00"));
    const Result result = manager.execute(value);
    EXPECT_EQ(result.status, TxStatus::RolledBack);
    EXPECT_NE(result.reason.find("injected after debit before credit"), std::string::npos);

    DbConnection connection(*config);
    AccountManager accounts(connection);
    EXPECT_EQ(accounts.balance(1).balance, Money::from_decimal("100.00"));
    EXPECT_EQ(accounts.balance(2).balance, Money::from_decimal("100.00"));
}

TEST_F(Phase5Fixture, I5LedgerReplayMatchesBalances) {
    reset(*config, {{1, "100.00"}, {2, "100.00"}, {3, "100.00"}});
    ConnectionPool connections(*config, 1);
    LockManager locks;
    TxnLog log(*config);
    TransactionManager manager(connections, locks, log, false);
    Request deposit = request("i5-deposit", TxType::Deposit, 1, 0, Money::from_decimal("5.00"));
    Request withdraw = request("i5-withdraw", TxType::Withdraw, 2, 0, Money::from_decimal("3.00"));
    Request transfer = request("i5-transfer", TxType::Transfer, 1, 3, Money::from_decimal("2.00"));
    ASSERT_EQ(manager.execute(deposit).status, TxStatus::Committed);
    ASSERT_EQ(manager.execute(withdraw).status, TxStatus::Committed);
    ASSERT_EQ(manager.execute(transfer).status, TxStatus::Committed);

    DbConnection connection(*config);
    std::map<int, Money> baseline;
    for (int account_id = 1; account_id <= 50; ++account_id) {
        baseline[account_id] = Money::from_decimal("100000.00");
    }
    baseline[1] = Money::from_decimal("100.00");
    baseline[2] = Money::from_decimal("100.00");
    baseline[3] = Money::from_decimal("100.00");
    const ConsistencyReport report = ConsistencyChecker(connection).check(baseline);
    std::cout << "I5 passed=" << report.passed
              << " i1=" << report.i1_total_conserved
              << " i3=" << report.i3_final_status
              << " i4=" << report.i4_no_pending
              << " i5=" << report.i5_ledger_matches
              << " actual=" << report.actual_total_minor
              << " expected=" << report.expected_total_minor
              << " mismatches=" << report.ledger_mismatches
              << " pending=" << report.pending
              << " status=" << report.status_violations << '\n';
    EXPECT_TRUE(report.passed);
    EXPECT_TRUE(report.i1_total_conserved);
    EXPECT_TRUE(report.i2_no_negative_balance);
    EXPECT_TRUE(report.i3_final_status);
    EXPECT_TRUE(report.i4_no_pending);
    EXPECT_TRUE(report.i5_ledger_matches);
}

TEST_F(Phase5Fixture, RecoveryResolvesPendingWithoutLedgerAsCrashRollback) {
    reset(*config, {{1, "100.00"}});
    DbConnection connection(*config);
    auto statement = connection.prepare(
        "INSERT INTO transaction_log (request_id, status) VALUES (?, 'PENDING')");
    std::string request_id = "recovery-crash";
    unsigned long request_length = static_cast<unsigned long>(request_id.size());
    MYSQL_BIND request_bind{};
    request_bind.buffer_type = MYSQL_TYPE_STRING;
    request_bind.buffer = request_id.data();
    request_bind.buffer_length = request_length;
    request_bind.length = &request_length;
    std::array<MYSQL_BIND, 1> params{request_bind};
    statement.bind_params(params);
    statement.execute();

    const RecoveryReport report = Recovery(connection).run();
    EXPECT_TRUE(report.succeeded);
    ASSERT_EQ(report.entries.size(), 1U);
    EXPECT_EQ(report.entries[0].status, TxStatus::RolledBack);
    EXPECT_EQ(report.entries[0].reason, "RECOVERY_CRASH");
}

}  // namespace
}  // namespace bank
