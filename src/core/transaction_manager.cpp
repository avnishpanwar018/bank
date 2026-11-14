#include "bank/core/transaction_manager.hpp"

#include "bank/core/account_manager.hpp"

#include <functional>
#include <sstream>
#include <thread>
#include <utility>

namespace bank {

TransactionManager::TransactionManager(ConnectionPool& connections, LockManager& lock_manager,
                                       TxnLog& log, bool unsafe, FaultInjector fault_injector)
    : connections_(connections), lock_manager_(lock_manager), log_(log), unsafe_(unsafe),
      fault_injector_(std::move(fault_injector)) {}

std::shared_ptr<std::mutex> TransactionManager::request_mutex(const std::string& request_id) {
    std::lock_guard lock(request_registry_mutex_);
    auto [iterator, inserted] = request_mutexes_.try_emplace(request_id,
                                                               std::make_shared<std::mutex>());
    static_cast<void>(inserted);
    return iterator->second;
}

Result TransactionManager::execute(Request& request) {
    if (request.request_id.empty() || request.request_id.size() > 36) {
        return {TxStatus::RolledBack, "INVALID_REQUEST_ID", Money{}, 1};
    }
    auto request_guard = request_mutex(request.request_id);
    std::lock_guard request_lock(*request_guard);
    if (const auto existing = log_.find_final(request.request_id)) {
        return *existing;
    }

    const LogToken token = log_.write_pending(request.request_id, 1, thread_id());
    std::string last_reason;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (attempt > 1) {
            log_.update_attempt(token, attempt);
            std::this_thread::yield();
        }
        try {
            Result result = execute_attempt(request, token, attempt);
            result.attempts = attempt;
            return result;
        } catch (const DbError& error) {
            last_reason = error.what();
            if (!retryable(error) || attempt == 3) {
                const std::string reason = retryable(error)
                    ? "DB_DEADLOCK_EXHAUSTED" : "DB_ERROR: " + last_reason;
                log_.resolve(token, TxStatus::RolledBack, reason, attempt);
                return {TxStatus::RolledBack, reason, Money{}, attempt};
            }
        } catch (const std::exception& error) {
            last_reason = error.what();
            const std::string reason = "DB_ERROR: " + last_reason;
            log_.resolve(token, TxStatus::RolledBack, reason, attempt);
            return {TxStatus::RolledBack, reason, Money{}, attempt};
        }
    }
    log_.resolve(token, TxStatus::RolledBack, last_reason, 3);
    return {TxStatus::RolledBack, last_reason, Money{}, 3};
}

Result TransactionManager::execute_attempt(Request& request, LogToken token, int attempt) {
    static_cast<void>(token);
    Result result;
    {
        ConnectionPool::Lease lease = connections_.acquire();
        std::optional<LockManager::ScopedLocks> locks;
        if (!unsafe_) {
            if (request.type == TxType::Transfer) {
                locks.emplace(lock_manager_.lock({request.from, request.to}));
            } else {
                locks.emplace(lock_manager_.lock({request.from}));
            }
        }

        DbConnection& connection = lease.connection();
        bool transaction_started = false;
        try {
            connection.start_transaction();
            transaction_started = true;
            if (fault_injector_) {
                fault_injector_(FaultPoint::AfterBegin, connection, request, attempt);
            }
            AccountManager manager(connection, unsafe_, nullptr, [&] {
                if (fault_injector_) {
                    fault_injector_(FaultPoint::AfterDebitBeforeCredit, connection, request,
                                    attempt);
                }
            });
            result = manager.apply_in_transaction(request);
            if (result.status == TxStatus::RolledBack) {
                connection.rollback();
            } else {
                connection.commit();
            }
        } catch (...) {
            if (transaction_started) {
                try {
                    connection.rollback();
                } catch (...) {
                }
            }
            throw;
        }
    }
    if (result.status == TxStatus::RolledBack) {
        log_.resolve(token, TxStatus::RolledBack, result.reason, attempt);
        return result;
    }
    log_.resolve(token, TxStatus::Committed, {}, attempt);
    return result;
}

bool TransactionManager::retryable(const DbError& error) noexcept {
    return error.code() == 1213 || error.code() == 1205;
}

std::string TransactionManager::thread_id() {
    std::ostringstream output;
    output << std::hash<std::thread::id>{}(std::this_thread::get_id());
    return output.str();
}

}  // namespace bank
