#pragma once

#include "bank/concurrency/counters.hpp"
#include "bank/core/lock_manager.hpp"
#include "bank/db/connection_pool.hpp"
#include "bank/logging/txn_log.hpp"
#include "bank/types.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace bank {

enum class FaultPoint {
    AfterBegin,
    AfterDebitBeforeCredit,
};

class TransactionManager final {
public:
    using FaultInjector =
        std::function<void(FaultPoint, DbConnection&, const Request&, int)>;

    TransactionManager(ConnectionPool& connections, LockManager& lock_manager,
                       TxnLog& log, bool unsafe = false,
                       FaultInjector fault_injector = {});

    [[nodiscard]] Result execute(Request& request);

private:
    [[nodiscard]] std::shared_ptr<std::mutex> request_mutex(const std::string& request_id);
    [[nodiscard]] Result execute_attempt(Request& request, LogToken token, int attempt);
    [[nodiscard]] static bool retryable(const DbError& error) noexcept;
    [[nodiscard]] static std::string thread_id();

    ConnectionPool& connections_;
    LockManager& lock_manager_;
    TxnLog& log_;
    bool unsafe_;
    FaultInjector fault_injector_;
    std::mutex request_registry_mutex_;
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> request_mutexes_;
};

}  // namespace bank
