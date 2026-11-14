#include "bank/config.hpp"
#include "bank/core/account_manager.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace bank {
namespace {

class AccountManagerIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        DbConfig config;
        try {
            config = DbConfig::from_environment();
        } catch (const std::invalid_argument& error) {
            if (std::getenv("BANK_DB_USER") == nullptr) {
                GTEST_SKIP() << error.what();
            }
            throw;
        }

        config.database = "bank_test";
        connection_ = std::make_unique<DbConnection>(config);
        manager_ = std::make_unique<AccountManager>(*connection_);
        reset_database();
    }

    void reset_database() {
        execute("DELETE FROM transaction_log");
        execute("DELETE FROM transactions");
        set_account_balance(1, "100000.00");
        set_account_balance(2, "100000.00");
    }

    void set_account_balance(int account_id, const std::string& amount) {
        auto statement = connection_->prepare(
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

    void execute(const char* sql) {
        auto statement = connection_->prepare(sql);
        statement.execute();
    }

    [[nodiscard]] Result balance(int account_id) {
        return manager_->balance(account_id);
    }

    std::unique_ptr<DbConnection> connection_;
    std::unique_ptr<AccountManager> manager_;
};

TEST_F(AccountManagerIntegrationTest, F1DepositCommits) {
    set_account_balance(1, "50.00");
    const Result result = manager_->deposit("00000000-0000-0000-0000-000000000001", 1,
                                           Money::from_decimal("100.00"));
    EXPECT_EQ(result.status, TxStatus::Committed);
    EXPECT_EQ(balance(1).balance, Money::from_decimal("150.00"));
}

TEST_F(AccountManagerIntegrationTest, F2WithdrawCommits) { 
    set_account_balance(1, "100.00");
    const Result result = manager_->withdraw("00000000-0000-0000-0000-000000000002", 1,
                                            Money::from_decimal("30.00"));
    EXPECT_EQ(result.status, TxStatus::Committed);
    EXPECT_EQ(balance(1).balance, Money::from_decimal("70.00"));
}

TEST_F(AccountManagerIntegrationTest, F3InsufficientFundsRollsBack) {
    set_account_balance(1, "100.00");
    const Result result = manager_->withdraw("00000000-0000-0000-0000-000000000003", 1,
                                            Money::from_decimal("200.00"));
    EXPECT_EQ(result.status, TxStatus::RolledBack);
    EXPECT_EQ(result.reason, "INSUFFICIENT_FUNDS");
    EXPECT_EQ(balance(1).balance, Money::from_decimal("100.00"));
}

TEST_F(AccountManagerIntegrationTest, F4TransferIsAtomic) {
    set_account_balance(1, "100.00");
    set_account_balance(2, "100.00");
    const Result result = manager_->transfer("00000000-0000-0000-0000-000000000004", 1, 2,
                                            Money::from_decimal("40.00"));
    EXPECT_EQ(result.status, TxStatus::Committed);
    EXPECT_EQ(balance(1).balance, Money::from_decimal("60.00"));
    EXPECT_EQ(balance(2).balance, Money::from_decimal("140.00"));
}

TEST_F(AccountManagerIntegrationTest, F5MissingDestinationRollsBack) {
    set_account_balance(1, "100.00");
    const Result result = manager_->transfer("00000000-0000-0000-0000-000000000005", 1, 999999,
                                            Money::from_decimal("40.00"));
    EXPECT_EQ(result.status, TxStatus::RolledBack);
    EXPECT_EQ(result.reason, "ACCOUNT_NOT_FOUND");
    EXPECT_EQ(balance(1).balance, Money::from_decimal("100.00"));
}

TEST_F(AccountManagerIntegrationTest, F6NonPositiveAmountIsRejected) {
    const Result result = manager_->deposit("00000000-0000-0000-0000-000000000006", 1,
                                           Money::from_minor_units(0));
    EXPECT_EQ(result.status, TxStatus::RolledBack);
    EXPECT_EQ(result.reason, "INVALID_AMOUNT");
}

TEST_F(AccountManagerIntegrationTest, F7SelfTransferIsRejected) {
    const Result result = manager_->transfer("00000000-0000-0000-0000-000000000007", 1, 1,
                                            Money::from_decimal("1.00"));
    EXPECT_EQ(result.status, TxStatus::RolledBack);
    EXPECT_EQ(result.reason, "SAME_ACCOUNT");
}

TEST_F(AccountManagerIntegrationTest, F8DuplicateRequestAppliesOnce) {
    set_account_balance(1, "100.00");
    const std::string request_id = "00000000-0000-0000-0000-000000000008";
    const Result first = manager_->deposit(request_id, 1, Money::from_decimal("10.00"));
    const Result duplicate = manager_->deposit(request_id, 1, Money::from_decimal("10.00"));
    EXPECT_EQ(first.status, TxStatus::Committed);
    EXPECT_EQ(duplicate.status, TxStatus::Committed);
    EXPECT_EQ(duplicate.reason, "DUPLICATE_REQUEST");
    EXPECT_EQ(balance(1).balance, Money::from_decimal("110.00"));
}

TEST_F(AccountManagerIntegrationTest, F9BalanceReturnsCommittedValue) {
    set_account_balance(1, "321.45");
    const Result result = balance(1);
    EXPECT_EQ(result.status, TxStatus::Committed);
    EXPECT_EQ(result.balance, Money::from_decimal("321.45"));
}

}  // namespace
}  // namespace bank
