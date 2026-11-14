#include "bank/logging/txn_log.hpp"

#include <array>
#include <stdexcept>

namespace bank {
namespace {

MYSQL_BIND string_param(const std::string& value, unsigned long& length) {
    MYSQL_BIND bind{};
    length = static_cast<unsigned long>(value.size());
    bind.buffer_type = MYSQL_TYPE_STRING;
    bind.buffer = const_cast<char*>(value.data());
    bind.buffer_length = length;
    bind.length = &length;
    return bind;
}

MYSQL_BIND integer_param(std::int64_t& value) {
    MYSQL_BIND bind{};
    bind.buffer_type = MYSQL_TYPE_LONGLONG;
    bind.buffer = &value;
    bind.buffer_length = sizeof(value);
    return bind;
}

MYSQL_BIND tiny_param(int& value) {
    MYSQL_BIND bind{};
    bind.buffer_type = MYSQL_TYPE_TINY;
    bind.buffer = &value;
    bind.buffer_length = sizeof(value);
    return bind;
}

}  // namespace

TxnLog::TxnLog(DbConfig config) : config_(config), connection_(std::move(config)) {
    connection_.set_autocommit(true);
}

std::optional<Result> TxnLog::find_final(const std::string& request_id) {
    std::lock_guard lock(mutex_);
    auto statement = connection_.prepare(
        "SELECT status, reason FROM transaction_log "
        "WHERE request_id = ? AND status <> 'PENDING' "
        "ORDER BY log_id DESC LIMIT 1");
    unsigned long request_length = 0;
    auto request_bind = string_param(request_id, request_length);
    std::array<MYSQL_BIND, 1> params{request_bind};
    statement.bind_params(params);
    statement.execute();

    char status[16]{};
    char reason[256]{};
    unsigned long status_length = 0;
    unsigned long reason_length = 0;
    bool status_null = false;
    bool reason_null = false;
    MYSQL_BIND status_bind{};
    status_bind.buffer_type = MYSQL_TYPE_STRING;
    status_bind.buffer = status;
    status_bind.buffer_length = sizeof(status);
    status_bind.length = &status_length;
    status_bind.is_null = &status_null;
    MYSQL_BIND reason_bind{};
    reason_bind.buffer_type = MYSQL_TYPE_STRING;
    reason_bind.buffer = reason;
    reason_bind.buffer_length = sizeof(reason);
    reason_bind.length = &reason_length;
    reason_bind.is_null = &reason_null;
    std::array<MYSQL_BIND, 2> results{status_bind, reason_bind};
    statement.bind_results(results);
    statement.store_result();
    if (statement.fetch() == MYSQL_NO_DATA) {
        return std::nullopt;
    }
    const bool committed = std::string_view(status, status_length) == "COMMITTED";
    return Result{committed ? TxStatus::Committed : TxStatus::RolledBack,
                  reason_null ? "DUPLICATE_REQUEST" : std::string(reason, reason_length),
                  Money{}, 1};
}

LogToken TxnLog::write_pending(const std::string& request_id, int attempt,
                               const std::string& thread_id_value) {
    std::lock_guard lock(mutex_);
    auto statement = connection_.prepare(
        "INSERT INTO transaction_log (request_id, status, thread_id, attempt) "
        "VALUES (?, 'PENDING', ?, ?)");
    unsigned long request_length = 0;
    unsigned long thread_length = 0;
    auto request_bind = string_param(request_id, request_length);
    auto thread_bind = string_param(thread_id_value, thread_length);
    auto attempt_bind = tiny_param(attempt);
    std::array<MYSQL_BIND, 3> params{request_bind, thread_bind, attempt_bind};
    statement.bind_params(params);
    statement.execute();
    return {static_cast<std::int64_t>(mysql_stmt_insert_id(statement.native_handle()))};
}

void TxnLog::update_attempt(LogToken token, int attempt) {
    std::lock_guard lock(mutex_);
    auto statement = connection_.prepare(
        "UPDATE transaction_log SET attempt = ? WHERE log_id = ? AND status = 'PENDING'");
    auto attempt_bind = tiny_param(attempt);
    std::int64_t log_id = token.log_id;
    auto log_bind = integer_param(log_id);
    std::array<MYSQL_BIND, 2> params{attempt_bind, log_bind};
    statement.bind_params(params);
    statement.execute();
    if (mysql_stmt_affected_rows(statement.native_handle()) != 1) {
        throw std::runtime_error("transaction log attempt update affected no row");
    }
}

void TxnLog::resolve(LogToken token, TxStatus status, const std::string& reason, int attempt) {
    std::lock_guard lock(mutex_);
    try {
        resolve_on(connection_, token, status, reason, attempt);
    } catch (const std::exception& primary_error) {
        try {
            DbConnection fallback(config_);
            fallback.set_autocommit(true);
            resolve_on(fallback, token, status, reason, attempt);
        } catch (const std::exception& fallback_error) {
            throw std::runtime_error(std::string("audit resolution failed: ") +
                                     primary_error.what() + "; fallback: " +
                                     fallback_error.what());
        }
    }
}

void TxnLog::resolve_on(DbConnection& connection, LogToken token, TxStatus status,
                        const std::string& reason, int attempt) {
    auto statement = connection.prepare(
        "UPDATE transaction_log SET status = ?, reason = ?, attempt = ? "
        "WHERE log_id = ? AND status = 'PENDING'");
    std::string status_text = status == TxStatus::Committed ? "COMMITTED" : "ROLLED_BACK";
    unsigned long status_length = 0;
    unsigned long reason_length = 0;
    auto status_bind = string_param(status_text, status_length);
    auto reason_bind = string_param(reason, reason_length);
    auto attempt_bind = tiny_param(attempt);
    std::int64_t log_id = token.log_id;
    auto log_bind = integer_param(log_id);
    std::array<MYSQL_BIND, 4> params{status_bind, reason_bind, attempt_bind, log_bind};
    statement.bind_params(params);
    statement.execute();
    if (mysql_stmt_affected_rows(statement.native_handle()) != 1) {
        auto lookup = connection.prepare(
            "SELECT status FROM transaction_log WHERE log_id = ?");
        std::int64_t log_id = token.log_id;
        auto id_lookup = integer_param(log_id);
        std::array<MYSQL_BIND, 1> lookup_params{id_lookup};
        lookup.bind_params(lookup_params);
        lookup.execute();
        char current_status[16]{};
        unsigned long current_length = 0;
        bool current_null = false;
        MYSQL_BIND current_bind{};
        current_bind.buffer_type = MYSQL_TYPE_STRING;
        current_bind.buffer = current_status;
        current_bind.buffer_length = sizeof(current_status);
        current_bind.length = &current_length;
        current_bind.is_null = &current_null;
        std::array<MYSQL_BIND, 1> current_results{current_bind};
        lookup.bind_results(current_results);
        lookup.store_result();
        if (lookup.fetch() == MYSQL_NO_DATA ||
            std::string_view(current_status, current_length) == "PENDING") {
            throw std::runtime_error("transaction log resolution affected no row");
        }
    }
}

}  // namespace bank
