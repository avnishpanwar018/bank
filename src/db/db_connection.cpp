#include "bank/db/db_connection.hpp"

#include <cstdlib>
#include <exception>
#include <mutex>
#include <utility>

namespace bank {
namespace {

void ensure_mysql_library() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        if (mysql_library_init(0, nullptr, nullptr) != 0) {
            throw std::runtime_error("mysql_library_init failed");
        }
    });
}

DbError make_connection_error(MYSQL* connection, const char* context) {
    return DbError(mysql_errno(connection), std::string(context) + ": " + mysql_error(connection));
}

DbError make_statement_error(MYSQL_STMT* statement, const char* context) {
    return DbError(mysql_stmt_errno(statement), std::string(context) + ": " + mysql_stmt_error(statement));
}

void close_statement(MYSQL_STMT* statement) noexcept {
    if (statement != nullptr && mysql_stmt_close(statement) != 0) {
        std::terminate();
    }
}

}  // namespace

DbError::DbError(unsigned int code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

PreparedStatement::PreparedStatement(MYSQL_STMT* statement) noexcept
    : statement_(statement) {}

PreparedStatement::~PreparedStatement() noexcept {
    close_statement(statement_);
}

PreparedStatement::PreparedStatement(PreparedStatement&& other) noexcept
    : statement_(std::exchange(other.statement_, nullptr)) {}

PreparedStatement& PreparedStatement::operator=(PreparedStatement&& other) noexcept {
    if (this != &other) {
        close_statement(statement_);
        statement_ = std::exchange(other.statement_, nullptr);
    }
    return *this;
}

void PreparedStatement::bind_params(std::span<MYSQL_BIND> params) {
    if (params.empty()) {
        return;
    }
    if (mysql_stmt_bind_param(statement_, params.data()) != 0) {
        throw make_statement_error(statement_, "mysql_stmt_bind_param");
    }
}

void PreparedStatement::bind_results(std::span<MYSQL_BIND> results) {
    if (results.empty()) {
        return;
    }
    if (mysql_stmt_bind_result(statement_, results.data()) != 0) {
        throw make_statement_error(statement_, "mysql_stmt_bind_result");
    }
}

void PreparedStatement::execute() {
    if (mysql_stmt_execute(statement_) != 0) {
        throw make_statement_error(statement_, "mysql_stmt_execute");
    }
}

void PreparedStatement::store_result() {
    if (mysql_stmt_store_result(statement_) != 0) {
        throw make_statement_error(statement_, "mysql_stmt_store_result");
    }
}

int PreparedStatement::fetch() {
    const int result = mysql_stmt_fetch(statement_);
    if (result != 0 && result != MYSQL_NO_DATA) {
        throw make_statement_error(statement_, "mysql_stmt_fetch");
    }
    return result;
}

DbConnection::DbConnection(DbConfig config) : config_(std::move(config)) {
    ensure_mysql_library();
    connection_ = mysql_init(nullptr);
    if (connection_ == nullptr) {
        throw DbError(0, "mysql_init failed");
    }
    if (mysql_real_connect(connection_, config_.host.c_str(), config_.user.c_str(),
                           config_.password.c_str(), config_.database.c_str(), config_.port,
                           nullptr, 0) == nullptr) {
        DbError error = make_connection_error(connection_, "mysql_real_connect");
        mysql_close(connection_);
        connection_ = nullptr;
        throw error;
    }
}

DbConnection::~DbConnection() noexcept {
    if (connection_ != nullptr) {
        mysql_close(connection_);
    }
}

PreparedStatement DbConnection::prepare(std::string_view sql) {
    MYSQL_STMT* statement = mysql_stmt_init(connection_);
    if (statement == nullptr) {
        throw make_connection_error(connection_, "mysql_stmt_init");
    }
    if (mysql_stmt_prepare(statement, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        DbError error = make_statement_error(statement, "mysql_stmt_prepare");
        close_statement(statement);
        throw error;
    }
    return PreparedStatement(statement);
}

void DbConnection::start_transaction() {
    constexpr char sql[] = "START TRANSACTION";
    if (mysql_real_query(connection_, sql, sizeof(sql) - 1) != 0) {
        throw make_connection_error(connection_, "START TRANSACTION");
    }
}

void DbConnection::commit() {
    if (mysql_commit(connection_) != 0) {
        throw make_connection_error(connection_, "COMMIT");
    }
}

void DbConnection::rollback() {
    if (mysql_rollback(connection_) != 0) {
        throw make_connection_error(connection_, "ROLLBACK");
    }
}

void DbConnection::set_autocommit(bool enabled) {
    if (mysql_autocommit(connection_, enabled ? 1 : 0) != 0) {
        throw make_connection_error(connection_, "mysql_autocommit");
    }
}

}  // namespace bank
