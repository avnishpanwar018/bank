#pragma once

#include "bank/config.hpp"

#if __has_include(<mysql.h>)
#include <mysql.h>
#else
#include <mysql/mysql.h>
#endif

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bank {

class DbError final : public std::runtime_error {
public:
    DbError(unsigned int code, std::string message);

    [[nodiscard]] unsigned int code() const noexcept { return code_; }

private:
    unsigned int code_;
};

class PreparedStatement final {
public:
    PreparedStatement() noexcept = default;
    explicit PreparedStatement(MYSQL_STMT* statement) noexcept;
    ~PreparedStatement() noexcept;

    PreparedStatement(const PreparedStatement&) = delete;
    PreparedStatement& operator=(const PreparedStatement&) = delete;
    PreparedStatement(PreparedStatement&& other) noexcept;
    PreparedStatement& operator=(PreparedStatement&& other) noexcept;

    void bind_params(std::span<MYSQL_BIND> params);
    void bind_results(std::span<MYSQL_BIND> results);
    void execute();
    void store_result();
    [[nodiscard]] int fetch();
    [[nodiscard]] MYSQL_STMT* native_handle() const noexcept { return statement_; }

private:
    MYSQL_STMT* statement_ = nullptr;
};

class DbConnection final {
public:
    explicit DbConnection(DbConfig config);
    ~DbConnection() noexcept;

    DbConnection(const DbConnection&) = delete;
    DbConnection& operator=(const DbConnection&) = delete;
    DbConnection(DbConnection&&) = delete;
    DbConnection& operator=(DbConnection&&) = delete;

    [[nodiscard]] PreparedStatement prepare(std::string_view sql);
    void start_transaction();
    void commit();
    void rollback();
    void set_autocommit(bool enabled);

    [[nodiscard]] const DbConfig& config() const noexcept { return config_; }
    [[nodiscard]] MYSQL* native_handle() const noexcept { return connection_; }

private:
    MYSQL* connection_ = nullptr;
    DbConfig config_;
};

}  // namespace bank
