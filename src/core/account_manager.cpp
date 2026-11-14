#include "bank/core/account_manager.hpp"

#include "bank/core/lock_manager.hpp"

#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace bank {
namespace {

constexpr std::string_view kInvalidAmount = "INVALID_AMOUNT";
constexpr std::string_view kAccountNotFound = "ACCOUNT_NOT_FOUND";
constexpr std::string_view kAccountNotActive = "ACCOUNT_NOT_ACTIVE";
constexpr std::string_view kInsufficientFunds = "INSUFFICIENT_FUNDS";
constexpr std::string_view kSameAccount = "SAME_ACCOUNT";
constexpr std::string_view kInvalidRequestId = "INVALID_REQUEST_ID";

struct AccountRow {
    int account_id = 0;
    char balance_text[64]{};
    unsigned long balance_length = 0;
    bool balance_is_null = false;
    char status[16]{};
    unsigned long status_length = 0;
    bool status_is_null = false;

    [[nodiscard]] Money balance() const {
        if (balance_is_null) {
            throw std::runtime_error("account balance was unexpectedly NULL");
        }
        return Money::from_decimal(std::string_view(balance_text, balance_length));
    }

    [[nodiscard]] std::string status_value() const {
        if (status_is_null) {
            throw std::runtime_error("account status was unexpectedly NULL");
        }
        return {status, status_length};
    }
};

MYSQL_BIND integer_param(int& value) {
    MYSQL_BIND bind{};
    bind.buffer_type = MYSQL_TYPE_LONG;
    bind.buffer = &value;
    bind.buffer_length = sizeof(value);
    return bind;
}

MYSQL_BIND string_param(const std::string& value, unsigned long& length) {
    MYSQL_BIND bind{};
    length = static_cast<unsigned long>(value.size());
    bind.buffer_type = MYSQL_TYPE_STRING;
    bind.buffer = const_cast<char*>(value.data());
    bind.buffer_length = length;
    bind.length = &length;
    return bind;
}

MYSQL_BIND null_param() {
    MYSQL_BIND bind{};
    bind.buffer_type = MYSQL_TYPE_NULL;
    return bind;
}

std::array<MYSQL_BIND, 3> account_results(AccountRow& row) {
    std::array<MYSQL_BIND, 3> results{};
    results[0].buffer_type = MYSQL_TYPE_LONG;
    results[0].buffer = &row.account_id;
    results[0].buffer_length = sizeof(row.account_id);
    results[1].buffer_type = MYSQL_TYPE_STRING;
    results[1].buffer = row.balance_text;
    results[1].buffer_length = sizeof(row.balance_text);
    results[1].length = &row.balance_length;
    results[1].is_null = &row.balance_is_null;
    results[2].buffer_type = MYSQL_TYPE_STRING;
    results[2].buffer = row.status;
    results[2].buffer_length = sizeof(row.status);
    results[2].length = &row.status_length;
    results[2].is_null = &row.status_is_null;
    return results;
}

bool fetch_account(PreparedStatement& statement, AccountRow& row) {
    auto results = account_results(row);
    statement.bind_results(results);
    statement.store_result();
    return statement.fetch() != MYSQL_NO_DATA;
}

bool fetch_account_by_id(DbConnection& connection, int account_id, AccountRow& row,
                         bool lock_row) {
    const char* sql = lock_row
        ? "SELECT account_id, balance, status FROM accounts WHERE account_id = ? FOR UPDATE"
        : "SELECT account_id, balance, status FROM accounts WHERE account_id = ?";
    auto statement = connection.prepare(sql);
    auto id_param = integer_param(account_id);
    std::array<MYSQL_BIND, 1> params{id_param};
    statement.bind_params(params);
    statement.execute();
    return fetch_account(statement, row);
}

std::string sql_type(TxType type) {
    switch (type) {
    case TxType::Deposit:
        return "DEPOSIT";
    case TxType::Withdraw:
        return "WITHDRAW";
    case TxType::Transfer:
        return "TRANSFER";
    case TxType::Balance:
        return "BALANCE";
    }
    throw std::invalid_argument("unknown transaction type");
}

}  // namespace

AccountManager::AccountManager(DbConnection& connection, bool unsafe, LockManager* lock_manager,
                               std::function<void()> after_debit_hook)
    : connection_(connection), config_(connection.config()), unsafe_(unsafe),
      lock_manager_(lock_manager), after_debit_hook_(std::move(after_debit_hook)) {}

Result AccountManager::apply_in_transaction(const Request& request) {
    if (request.type == TxType::Transfer) {
        if (!request.amount.is_positive()) {
            return {TxStatus::RolledBack, std::string(kInvalidAmount), Money{}, 1};
        }
        if (request.from == request.to) {
            return {TxStatus::RolledBack, std::string(kSameAccount), Money{}, 1};
        }
        return perform_transfer(request.request_id, request.from, request.to, request.amount);
    }
    if (request.type == TxType::Deposit || request.type == TxType::Withdraw) {
        if (!request.amount.is_positive()) {
            return {TxStatus::RolledBack, std::string(kInvalidAmount), Money{}, 1};
        }
        return perform_single_account(request.request_id, request.type, request.from,
                                      request.amount);
    }
    return balance_in_transaction(request.from);
}

Result AccountManager::balance_in_transaction(int account_id) {
    const char* select_sql = unsafe_
        ? "SELECT account_id, balance, status FROM accounts WHERE account_id = ?"
        : "SELECT account_id, balance, status FROM accounts WHERE account_id = ? FOR UPDATE";
    auto statement = connection_.prepare(select_sql);
    auto id_param = integer_param(account_id);
    std::array<MYSQL_BIND, 1> params{id_param};
    statement.bind_params(params);
    statement.execute();
    AccountRow row;
    if (!fetch_account(statement, row)) {
        return {TxStatus::RolledBack, std::string(kAccountNotFound), Money{}, 1};
    }
    if (row.status_value() != "ACTIVE") {
        return {TxStatus::RolledBack, std::string(kAccountNotActive), Money{}, 1};
    }
    return {TxStatus::Committed, {}, row.balance(), 1};
}

Result AccountManager::deposit(const std::string& request_id, int account_id, Money amount) {
    return execute_amount_operation(request_id, TxType::Deposit, account_id, 0, amount);
}

Result AccountManager::withdraw(const std::string& request_id, int account_id, Money amount) {
    return execute_amount_operation(request_id, TxType::Withdraw, account_id, 0, amount);
}

Result AccountManager::transfer(const std::string& request_id, int from_account,
                                int to_account, Money amount) {
    return execute_amount_operation(request_id, TxType::Transfer, from_account, to_account, amount);
}

Result AccountManager::execute_amount_operation(const std::string& request_id, TxType type,
                                                int first_account, int second_account,
                                                Money amount) {
    if (request_id.empty() || request_id.size() > 36) {
        return {TxStatus::RolledBack, std::string(kInvalidRequestId), Money{}, 1};
    }
    Result duplicate;
    if (duplicate_result(request_id, duplicate)) {
        return duplicate;
    }

    Result operation_result;
    std::string failure_reason;
    std::string database_failure;
    try {
        write_pending(request_id);
    } catch (const std::exception& error) {
        return finish_database_failure(request_id, error.what());
    }
    {
        std::optional<LockManager::ScopedLocks> locks;
        try {
            if (!unsafe_ && lock_manager_ != nullptr) {
                if (type == TxType::Transfer) {
                    locks.emplace(lock_manager_->lock({first_account, second_account}));
                } else {
                    locks.emplace(lock_manager_->lock({first_account}));
                }
            }

            connection_.start_transaction();
            if (!amount.is_positive()) {
                connection_.rollback();
                failure_reason = std::string(kInvalidAmount);
            } else if (type == TxType::Transfer && first_account == second_account) {
                connection_.rollback();
                failure_reason = std::string(kSameAccount);
            } else {
                if (type == TxType::Transfer) {
                    operation_result = perform_transfer(request_id, first_account, second_account,
                                                        amount);
                } else {
                    operation_result = perform_single_account(request_id, type, first_account,
                                                              amount);
                }
                if (operation_result.status == TxStatus::RolledBack) {
                    connection_.rollback();
                    failure_reason = operation_result.reason;
                } else {
                    connection_.commit();
                }
            }
        } catch (const std::exception& error) {
            database_failure = error.what();
            try {
                connection_.rollback();
            } catch (const std::exception& rollback_error) {
                database_failure += "; rollback failed: ";
                database_failure += rollback_error.what();
            }
        }
    }

    if (!database_failure.empty()) {
        return finish_database_failure(request_id, database_failure);
    }
    if (!failure_reason.empty()) {
        if (amount.is_positive()) {
            record_rolled_back_transaction(request_id, type, first_account, second_account,
                                           amount, failure_reason);
        }
        return finish_failure(request_id, failure_reason);
    }

    try {
        resolve_pending(request_id, TxStatus::Committed, {});
        return {TxStatus::Committed, {}, Money{}, 1};
    } catch (const std::exception& error) {
        return {TxStatus::Committed, std::string("COMMITTED_LOG_ERROR: ") + error.what(), Money{}, 1};
    }
}

Result AccountManager::perform_single_account(const std::string& request_id, TxType type,
                                              int account_id, Money amount) {
    const char* select_sql = unsafe_
        ? "SELECT account_id, balance, status FROM accounts WHERE account_id = ?"
        : "SELECT account_id, balance, status FROM accounts WHERE account_id = ? FOR UPDATE";
    auto statement = connection_.prepare(select_sql);
    auto id_param = integer_param(account_id);
    std::array<MYSQL_BIND, 1> params{id_param};
    statement.bind_params(params);
    statement.execute();

    AccountRow row;
    if (!fetch_account(statement, row)) {
        return {TxStatus::RolledBack, std::string(kAccountNotFound), Money{}, 1};
    }
    if (row.status_value() != "ACTIVE") {
        return {TxStatus::RolledBack, std::string(kAccountNotActive), Money{}, 1};
    }
    if (type == TxType::Withdraw && row.balance().minor_units() < amount.minor_units()) {
        return {TxStatus::RolledBack, std::string(kInsufficientFunds), Money{}, 1};
    }

    if (unsafe_) {
        const std::int64_t current = row.balance().minor_units();
        const std::int64_t delta = amount.minor_units();
        if (type == TxType::Deposit && current > std::numeric_limits<std::int64_t>::max() - delta) {
            return {TxStatus::RolledBack, std::string(kInvalidAmount), Money{}, 1};
        }
        set_balance(account_id, Money::from_minor_units(
            type == TxType::Deposit ? current + delta : current - delta));
    } else {
        const std::string amount_text = amount.to_decimal();
        const char* update_sql = type == TxType::Deposit
            ? "UPDATE accounts SET balance = balance + CAST(? AS DECIMAL(15,2)), "
              "version = version + 1 WHERE account_id = ?"
            : "UPDATE accounts SET balance = balance - CAST(? AS DECIMAL(15,2)), "
              "version = version + 1 WHERE account_id = ?";
        auto update = connection_.prepare(update_sql);
        unsigned long amount_length = 0;
        auto amount_param = string_param(amount_text, amount_length);
        auto update_id_param = integer_param(account_id);
        std::array<MYSQL_BIND, 2> update_params{amount_param, update_id_param};
        update.bind_params(update_params);
        update.execute();
    }
    const int from_account = type == TxType::Withdraw ? account_id : 0;
    const int to_account = type == TxType::Deposit ? account_id : 0;
    insert_transaction(connection_, request_id, type, from_account, to_account, amount, false);
    return {TxStatus::Committed, {}, Money{}, 1};
}

Result AccountManager::perform_transfer(const std::string& request_id, int from_account,
                                        int to_account, Money amount) {
    int lower_account = from_account < to_account ? from_account : to_account;
    int higher_account = from_account < to_account ? to_account : from_account;
    AccountRow first;
    AccountRow second;
    const bool first_found = fetch_account_by_id(connection_, lower_account, first, !unsafe_);
    const bool second_found = fetch_account_by_id(connection_, higher_account, second, !unsafe_);
    if (!first_found || !second_found) {
        return {TxStatus::RolledBack, std::string(kAccountNotFound), Money{}, 1};
    }
    if (first.status_value() != "ACTIVE" || second.status_value() != "ACTIVE") {
        return {TxStatus::RolledBack, std::string(kAccountNotActive), Money{}, 1};
    }

    const AccountRow& source = from_account == first.account_id ? first : second;
    if (source.balance().minor_units() < amount.minor_units()) {
        return {TxStatus::RolledBack, std::string(kInsufficientFunds), Money{}, 1};
    }

    if (unsafe_) {
        const std::int64_t source_balance = source.balance().minor_units();
        const std::int64_t destination_balance =
            (from_account == first.account_id ? second : first).balance().minor_units();
        if (destination_balance > std::numeric_limits<std::int64_t>::max() - amount.minor_units()) {
            return {TxStatus::RolledBack, std::string(kInvalidAmount), Money{}, 1};
        }
        set_balance(from_account, Money::from_minor_units(source_balance - amount.minor_units()));
        set_balance(to_account,
                    Money::from_minor_units(destination_balance + amount.minor_units()));
    } else {
        const std::string amount_text = amount.to_decimal();
        update_balance(from_account, amount_text, false);
        if (after_debit_hook_) {
            after_debit_hook_();
        }
        update_balance(to_account, amount_text, true);
    }
    insert_transaction(connection_, request_id, TxType::Transfer, from_account, to_account,
                       amount, false);
    return {TxStatus::Committed, {}, Money{}, 1};
}

void AccountManager::update_balance(int account_id, const std::string& amount_text, bool credit) {
    const char* sql = credit
        ? "UPDATE accounts SET balance = balance + CAST(? AS DECIMAL(15,2)), "
          "version = version + 1 WHERE account_id = ?"
        : "UPDATE accounts SET balance = balance - CAST(? AS DECIMAL(15,2)), "
          "version = version + 1 WHERE account_id = ?";
    auto statement = connection_.prepare(sql);
    unsigned long amount_length = 0;
    auto amount_param = string_param(amount_text, amount_length);
    auto id_param = integer_param(account_id);
    std::array<MYSQL_BIND, 2> params{amount_param, id_param};
    statement.bind_params(params);
    statement.execute();
}

void AccountManager::set_balance(int account_id, Money balance) {
    auto statement = connection_.prepare(
        "UPDATE accounts SET balance = CAST(? AS DECIMAL(15,2)), "
        "version = version + 1 WHERE account_id = ?");
    const std::string balance_text = balance.to_decimal();
    unsigned long balance_length = 0;
    auto balance_param = string_param(balance_text, balance_length);
    auto id_param = integer_param(account_id);
    std::array<MYSQL_BIND, 2> params{balance_param, id_param};
    statement.bind_params(params);
    statement.execute();
}

void AccountManager::insert_transaction(DbConnection& connection, const std::string& request_id,
                                        TxType type, int from_account, int to_account,
                                        Money amount, bool rolled_back) {
    auto statement = connection.prepare(
        "INSERT INTO transactions "
        "(request_id, type, from_account, to_account, amount, status) "
        "VALUES (?, ?, ?, ?, CAST(? AS DECIMAL(15,2)), ?)");
    std::string type_text = sql_type(type);
    std::string amount_text = amount.to_decimal();
    std::string status_text = rolled_back ? "ROLLED_BACK" : "COMMITTED";
    unsigned long request_length = 0;
    unsigned long type_length = 0;
    unsigned long amount_length = 0;
    unsigned long status_length = 0;
    auto request_param = string_param(request_id, request_length);
    auto type_param = string_param(type_text, type_length);
    auto amount_param = string_param(amount_text, amount_length);
    auto status_param = string_param(status_text, status_length);
    auto from_param = from_account == 0 ? null_param() : integer_param(from_account);
    auto to_param = to_account == 0 ? null_param() : integer_param(to_account);
    std::array<MYSQL_BIND, 6> params{
        request_param, type_param, from_param, to_param, amount_param, status_param};
    statement.bind_params(params);
    statement.execute();
}

void AccountManager::record_rolled_back_transaction(const std::string& request_id, TxType type,
                                                    int first_account, int second_account,
                                                    Money amount, const std::string& reason) {
    DbConnection audit(config_);
    audit.set_autocommit(true);
    int from_account = 0;
    int to_account = 0;
    if (reason != kAccountNotFound) {
        if (type == TxType::Withdraw) {
            from_account = first_account;
        } else if (type == TxType::Deposit) {
            to_account = first_account;
        } else {
            from_account = first_account;
            to_account = second_account;
        }
    }
    insert_transaction(audit, request_id, type, from_account, to_account, amount, true);
}

Result AccountManager::balance(int account_id) {
    Result result;
    std::string failure_reason;
    {
        std::optional<LockManager::ScopedLocks> locks;
        try {
            if (!unsafe_ && lock_manager_ != nullptr) {
                locks.emplace(lock_manager_->lock({account_id}));
            }
            connection_.start_transaction();
            const char* select_sql = unsafe_
                ? "SELECT account_id, balance, status FROM accounts WHERE account_id = ?"
                : "SELECT account_id, balance, status FROM accounts WHERE account_id = ? FOR UPDATE";
            auto statement = connection_.prepare(select_sql);
            auto id_param = integer_param(account_id);
            std::array<MYSQL_BIND, 1> params{id_param};
            statement.bind_params(params);
            statement.execute();
            AccountRow row;
            if (!fetch_account(statement, row)) {
                connection_.rollback();
                failure_reason = std::string(kAccountNotFound);
            } else if (row.status_value() != "ACTIVE") {
                connection_.rollback();
                failure_reason = std::string(kAccountNotActive);
            } else {
                result = {TxStatus::Committed, {}, row.balance(), 1};
                connection_.commit();
            }
        } catch (const std::exception& error) {
            failure_reason = error.what();
            try {
                connection_.rollback();
            } catch (const std::exception& rollback_error) {
                failure_reason += "; rollback failed: ";
                failure_reason += rollback_error.what();
            }
        }
    }
    if (!failure_reason.empty()) {
        return {TxStatus::RolledBack, failure_reason, Money{}, 1};
    }
    return result;
}

bool AccountManager::duplicate_result(const std::string& request_id, Result& result) {
    auto statement = connection_.prepare(
        "SELECT status FROM transactions WHERE request_id = ?");
    unsigned long request_length = 0;
    auto request_param = string_param(request_id, request_length);
    std::array<MYSQL_BIND, 1> params{request_param};
    statement.bind_params(params);
    statement.execute();
    char status[16]{};
    unsigned long status_length = 0;
    bool status_is_null = false;
    MYSQL_BIND output{};
    output.buffer_type = MYSQL_TYPE_STRING;
    output.buffer = status;
    output.buffer_length = sizeof(status);
    output.length = &status_length;
    output.is_null = &status_is_null;
    std::array<MYSQL_BIND, 1> results{output};
    statement.bind_results(results);
    statement.store_result();
    if (statement.fetch() == MYSQL_NO_DATA) {
        auto log_statement = connection_.prepare(
            "SELECT status FROM transaction_log "
            "WHERE request_id = ? AND status <> 'PENDING' "
            "ORDER BY log_id DESC LIMIT 1");
        auto log_request_param = string_param(request_id, request_length);
        std::array<MYSQL_BIND, 1> log_params{log_request_param};
        log_statement.bind_params(log_params);
        log_statement.execute();
        std::array<MYSQL_BIND, 1> log_results{output};
        log_statement.bind_results(log_results);
        log_statement.store_result();
        if (log_statement.fetch() == MYSQL_NO_DATA) {
            return false;
        }
    }
    result.status = std::string_view(status, status_length) == "COMMITTED"
        ? TxStatus::Committed : TxStatus::RolledBack;
    result.reason = "DUPLICATE_REQUEST";
    result.balance = Money{};
    result.attempts = 1;
    return true;
}

void AccountManager::write_pending(const std::string& request_id) {
    DbConnection audit(config_);
    audit.set_autocommit(true);
    auto statement = audit.prepare(
        "INSERT INTO transaction_log (request_id, status) VALUES (?, 'PENDING')");
    unsigned long request_length = 0;
    auto request_param = string_param(request_id, request_length);
    std::array<MYSQL_BIND, 1> params{request_param};
    statement.bind_params(params);
    statement.execute();
    if (mysql_stmt_affected_rows(statement.native_handle()) != 1) {
        throw std::runtime_error("transaction log PENDING insert did not affect one row");
    }
}

void AccountManager::resolve_pending(const std::string& request_id, TxStatus status,
                                     const std::string& reason) {
    DbConnection audit(config_);
    audit.set_autocommit(true);
    auto statement = audit.prepare(
        "UPDATE transaction_log SET status = ?, reason = ? "
        "WHERE request_id = ? AND status = 'PENDING'");
    std::string status_text = status == TxStatus::Committed ? "COMMITTED" : "ROLLED_BACK";
    unsigned long status_length = 0;
    unsigned long reason_length = 0;
    unsigned long request_length = 0;
    auto status_param = string_param(status_text, status_length);
    auto reason_param = string_param(reason, reason_length);
    auto request_param = string_param(request_id, request_length);
    std::array<MYSQL_BIND, 3> params{status_param, reason_param, request_param};
    statement.bind_params(params);
    statement.execute();
    if (mysql_stmt_affected_rows(statement.native_handle()) != 1) {
        throw std::runtime_error("transaction log did not contain exactly one PENDING row");
    }
}

Result AccountManager::finish_failure(const std::string& request_id, const std::string& reason) {
    try {
        resolve_pending(request_id, TxStatus::RolledBack, reason);
        return {TxStatus::RolledBack, reason, Money{}, 1};
    } catch (const std::exception& error) {
        return {TxStatus::RolledBack, reason + "; log resolution failed: " + error.what(), Money{}, 1};
    }
}

Result AccountManager::finish_database_failure(const std::string& request_id,
                                               const std::string& reason) {
    return finish_failure(request_id, reason);
}

}  // namespace bank
