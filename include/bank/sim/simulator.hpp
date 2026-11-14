#pragma once

#include "bank/config.hpp"

#include <cstddef>
#include <cstdint>

namespace bank {

struct SimulatorOptions {
    std::size_t accounts = 50;
    std::size_t clients = 100;
    std::size_t operations_per_client = 100;
    std::size_t workers = 8;
    std::size_t queue_capacity = 256;
    std::uint64_t seed = 42;
    bool unsafe = false;
    bool verbose = false;
    bool check = false;
    bool recover = false;
    bool serve = false;
    std::uint16_t port = 8080;
    std::string baseline_file;
};

struct SimulationReport {
    std::uint64_t committed = 0;
    std::uint64_t rolled_back = 0;
    std::uint64_t total = 0;
    std::uint64_t lock_waits = 0;
    std::uint64_t service_latency_ns = 0;
};

class Simulator final {
public:
    Simulator(DbConfig config, SimulatorOptions options);

    [[nodiscard]] SimulationReport run();

private:
    DbConfig config_;
    SimulatorOptions options_;
};

}  // namespace bank
