#pragma once

#include <cstdint>
#include <string>

namespace bank {

struct DbConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3306;
    std::string user;
    std::string password;
    std::string database = "bank";

    static DbConfig from_environment();
};

}  // namespace bank
