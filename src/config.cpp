#include "bank/config.hpp"

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace bank {
namespace {

void set_process_environment(const std::string& name, const std::string& value) {
#ifdef _WIN32
    if (_putenv_s(name.c_str(), value.c_str()) != 0) {
        throw std::runtime_error("failed to set environment variable from .env: " + name);
    }
#else
    if (setenv(name.c_str(), value.c_str(), 0) != 0) {
        throw std::runtime_error("failed to set environment variable from .env: " + name);
    }
#endif
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string unquote(std::string value) {
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

void load_dotenv_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        std::string entry = trim(line);
        if (entry.empty() || entry.front() == '#') {
            continue;
        }
        if (entry.starts_with("export ")) {
            entry = trim(entry.substr(7));
        }
        const std::size_t separator = entry.find('=');
        if (separator == std::string::npos) {
            throw std::invalid_argument("invalid .env entry at line " + std::to_string(line_number));
        }
        const std::string name = trim(entry.substr(0, separator));
        std::string value = trim(entry.substr(separator + 1));
        if (name.empty()) {
            throw std::invalid_argument(".env entry has an empty variable name at line " +
                                        std::to_string(line_number));
        }
        if (const std::size_t comment = value.find(" #"); comment != std::string::npos) {
            value = trim(value.substr(0, comment));
        }
        if (std::getenv(name.c_str()) == nullptr) {
            set_process_environment(name, unquote(value));
        }
    }
}

void load_dotenv() {
    std::error_code error;
    std::filesystem::path directory = std::filesystem::current_path(error);
    if (error) {
        return;
    }
    while (true) {
        load_dotenv_file(directory / ".env");
        const std::filesystem::path parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }
}

std::string environment_value(const char* name, bool required) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        if (required) {
            throw std::invalid_argument(std::string("missing required environment variable: ") + name);
        }
        return {};
    }
    return value;
}

std::uint16_t parse_port(const std::string& value) {
    unsigned int parsed = 0;
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto [next, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || next != end || parsed == 0 || parsed > 65535) {
        throw std::invalid_argument("BANK_DB_PORT must be an integer from 1 to 65535");
    }
    return static_cast<std::uint16_t>(parsed);
}

}  // namespace

DbConfig DbConfig::from_environment() {
    load_dotenv();
    DbConfig config;
    const std::string host = environment_value("BANK_DB_HOST", false);
    const std::string port = environment_value("BANK_DB_PORT", false);
    const std::string database = environment_value("BANK_DB_NAME", false);

    if (!host.empty()) {
        config.host = host;
    }
    if (!port.empty()) {
        config.port = parse_port(port);
    }
    if (!database.empty()) {
        config.database = database;
    }
    config.user = environment_value("BANK_DB_USER", true);
    config.password = environment_value("BANK_DB_PASS", false);
    return config;
}

}  // namespace bank
