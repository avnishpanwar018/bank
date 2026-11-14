#pragma once

#include "bank/config.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace bank {

class DashboardServer final {
public:
    DashboardServer(DbConfig config, std::uint16_t port = 8080,
                     std::string web_root = "web",
                     std::string baseline_file = "results/baseline.txt");
    ~DashboardServer() noexcept;

    DashboardServer(const DashboardServer&) = delete;
    DashboardServer& operator=(const DashboardServer&) = delete;

    void start();
    void wait();
    void stop() noexcept;

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] bool is_running() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bank
