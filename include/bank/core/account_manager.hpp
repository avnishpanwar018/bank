#pragma once

#include "bank/db/db_connection.hpp"
#include "bank/types.hpp"

#include <string>
#include <functional>

namespace bank {

class LockManager;

class AccountManager final {
public:
    explicit AccountManager(DbConnection& connection, bool unsafe = false,
                            LockManager* lock_manager = nullptr,
                            std::function<void()> after_debit_hook = {});

    [[nodiscard]] Result deposit(const std::string& request_id,
                                 int account_id, Money amount);
    [[nodiscard]] Result withdraw(const std::string& request_id,
                                  int account_id, Money amount);
    [[nodiscard]] Result transfer(const std::string& request_id,
                                  int from_account, int to_account,
                                  Money amount);
    [[nodiscard]] Result balance(int account_id);
    [[nodiscard]] Result apply_in_transaction(const Request& request);
    [[nodiscard]] Result balance_in_transaction(int account_id);

private:
    [[nodiscard]] Result execute_amount_operation(const std::string& request_id,
                                                  TxType type, int first_account,
                                                  int second_account, Money amount);
    [[nodiscard]] Result finish_failure(const std::string& request_id,
                                        const std::string& reason);
    [[nodiscard]] Result finish_database_failure(const std::string& request_id,
                                                 const std::string& reason);
    [[nodiscard]] Result perform_single_account(const std::string& request_id, TxType type,
                                                int account_id, Money amount);
    [[nodiscard]] Result perform_transfer(const std::string& request_id, int from_account,
                                          int to_account, Money amount);
    void record_rolled_back_transaction(const std::string& request_id, TxType type,
                                        int first_account, int second_account, Money amount,
                                        const std::string& reason);
    [[nodiscard]] bool duplicate_result(const std::string& request_id,
                                        Result& result);
    void update_balance(int account_id, const std::string& amount_text, bool credit);
    void set_balance(int account_id, Money balance);
    void insert_transaction(DbConnection& connection, const std::string& request_id,
                            TxType type, int from_account, int to_account, Money amount,
                            bool rolled_back);
    void write_pending(const std::string& request_id);
    void resolve_pending(const std::string& request_id, TxStatus status,
                         const std::string& reason);

    DbConnection& connection_;
    DbConfig config_;
    bool unsafe_ = false;
    LockManager* lock_manager_ = nullptr;
    std::function<void()> after_debit_hook_;
};

}  // namespace bank
