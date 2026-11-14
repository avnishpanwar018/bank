#include "bank/config.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace bank {
namespace {

void clear_environment_value(const char* name) {
#ifdef _WIN32
    ASSERT_EQ(_putenv_s(name, ""), 0);
#else
    ASSERT_EQ(unsetenv(name), 0);
#endif
}

TEST(ConfigTest, LoadsDotenvWithoutOverridingExistingEnvironment) {
    if (std::getenv("BANK_DB_HOST") != nullptr || std::getenv("BANK_DB_PORT") != nullptr ||
        std::getenv("BANK_DB_USER") != nullptr || std::getenv("BANK_DB_PASS") != nullptr ||
        std::getenv("BANK_DB_NAME") != nullptr) {
        GTEST_SKIP() << "BANK_DB_* is already configured; dotenv isolation test skipped";
    }

    const std::filesystem::path dotenv = std::filesystem::current_path() / ".env";
    if (std::filesystem::exists(dotenv)) {
        GTEST_SKIP() << "test working directory already contains .env";
    }

    {
        std::ofstream file(dotenv);
        ASSERT_TRUE(file.is_open());
        file << "BANK_DB_HOST=dotenv-host\n"
             << "BANK_DB_PORT=3307\n"
             << "BANK_DB_USER=dotenv-user\n"
             << "BANK_DB_PASS=dotenv-pass\n"
             << "BANK_DB_NAME=dotenv-db\n";
    }

    const DbConfig config = DbConfig::from_environment();
    EXPECT_EQ(config.host, "dotenv-host");
    EXPECT_EQ(config.port, 3307);
    EXPECT_EQ(config.user, "dotenv-user");
    EXPECT_EQ(config.password, "dotenv-pass");
    EXPECT_EQ(config.database, "dotenv-db");

    std::error_code error;
    std::filesystem::remove(dotenv, error);
    ASSERT_FALSE(error) << error.message();
    clear_environment_value("BANK_DB_HOST");
    clear_environment_value("BANK_DB_PORT");
    clear_environment_value("BANK_DB_USER");
    clear_environment_value("BANK_DB_PASS");
    clear_environment_value("BANK_DB_NAME");
}

}  // namespace
}  // namespace bank
