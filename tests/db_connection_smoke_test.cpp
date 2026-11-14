#include "bank/config.hpp"
#include "bank/db/db_connection.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <stdexcept>

namespace bank {

TEST(DbConnectionSmokeTest, ConnectsAndSelectsOne) {
    DbConfig config;
    try {
        config = DbConfig::from_environment();
    } catch (const std::invalid_argument& error) {
        if (std::getenv("BANK_DB_USER") == nullptr) {
            GTEST_SKIP() << error.what();
        }
        throw;
    }

    DbConnection connection(config);
    auto statement = connection.prepare("SELECT 1");
    statement.execute();
    int value = 0;
    bool is_null = false;
    MYSQL_BIND output{};
    output.buffer_type = MYSQL_TYPE_LONG;
    output.buffer = &value;
    output.buffer_length = sizeof(value);
    output.is_null = &is_null;
    std::array<MYSQL_BIND, 1> results{output};
    statement.bind_results(results);
    statement.store_result();
    ASSERT_EQ(statement.fetch(), 0);
    EXPECT_FALSE(is_null);
    EXPECT_EQ(value, 1);
}

}  // namespace bank
