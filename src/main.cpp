#include "bank/sim/simulator.hpp"
#include "bank/tools/consistency_checker.hpp"
#include "bank/tools/recovery.hpp"
#include "bank/web/dashboard_server.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace bank {
namespace {

template <typename Integer>
Integer parse_integer(std::string_view value, const char* option) {
    Integer parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::invalid_argument(std::string("invalid value for ") + option);
    }
    return parsed;
}

void require_value(int index, int argc, const char* option) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string("missing value for ") + option);
    }
}

void print_help() {
    std::cout << "Usage: bank [--accounts N] [--clients N] [--ops N] "
                 "[--workers N] [--seed S] [--unsafe] [--verbose] [--check] "
                 "[--recover] [--baseline-file PATH] [--serve --port N]\n"
              << "  --verbose  print live queue, worker, throughput, and contention metrics\n"
              << "  --check    run consistency checks (requires --baseline-file)\n"
              << "  --recover  resolve PENDING transaction log rows and exit\n"
              << "  --serve    run the optional localhost web dashboard\n"
              << "  --port N   dashboard TCP port (default 8080)\n";
}

std::map<int, bank::Money> load_baseline(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::invalid_argument("cannot open baseline file: " + path);
    }
    std::map<int, bank::Money> baseline;
    int account_id = 0;
    std::int64_t minor_units = 0;
    while (input >> account_id >> minor_units) {
        if (account_id <= 0 || minor_units < 0 || !baseline.emplace(
                account_id, bank::Money::from_minor_units(minor_units)).second) {
            throw std::invalid_argument("invalid or duplicate baseline entry");
        }
    }
    if (!input.eof() || baseline.empty()) {
        throw std::invalid_argument("baseline file is empty or malformed");
    }
    return baseline;
}

std::optional<SimulatorOptions> parse_options(int argc, char* argv[]) {
    SimulatorOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--help") {
            print_help();
            return std::nullopt;
        }
        if (option == "--unsafe") {
            options.unsafe = true;
            continue;
        }
        if (option == "--verbose") {
            options.verbose = true;
            continue;
        }
        if (option == "--check") {
            options.check = true;
            continue;
        }
        if (option == "--recover") {
            options.recover = true;
            continue;
        }
        if (option == "--serve") {
            options.serve = true;
            continue;
        }
        require_value(index, argc, argv[index]);
        const std::string_view value = argv[++index];
        if (option == "--accounts") {
            options.accounts = parse_integer<std::size_t>(value, "--accounts");
        } else if (option == "--clients") {
            options.clients = parse_integer<std::size_t>(value, "--clients");
        } else if (option == "--ops") {
            options.operations_per_client = parse_integer<std::size_t>(value, "--ops");
        } else if (option == "--workers") {
            options.workers = parse_integer<std::size_t>(value, "--workers");
        } else if (option == "--seed") {
            options.seed = parse_integer<std::uint64_t>(value, "--seed");
        } else if (option == "--port") {
            const std::uint64_t port = parse_integer<std::uint64_t>(value, "--port");
            if (port == 0 || port > 65535) {
                throw std::invalid_argument("--port must be between 1 and 65535");
            }
            options.port = static_cast<std::uint16_t>(port);
        } else if (option == "--baseline-file") {
            options.baseline_file = std::string(value);
        } else {
            throw std::invalid_argument(std::string("unknown option: ") + std::string(option));
        }
    }
    return options;
}

void print_recovery_report(const char* prefix, const bank::RecoveryReport& report) {
    std::cout << prefix << ".pending_seen=" << report.pending_seen << '\n'
              << prefix << ".committed=" << report.committed << '\n'
              << prefix << ".rolled_back=" << report.rolled_back << '\n'
              << prefix << ".remaining_pending=" << report.remaining_pending << '\n';
    for (const std::string& error : report.errors) {
        std::cerr << prefix << ".error=" << error << '\n';
    }
}

void print_consistency_report(const bank::ConsistencyReport& report) {
    std::cout << "I1=" << (report.i1_total_conserved ? "PASS" : "FAIL")
              << " actual_minor=" << report.actual_total_minor
              << " expected_minor=" << report.expected_total_minor << '\n'
              << "I2=" << (report.i2_no_negative_balance ? "PASS" : "FAIL")
              << " negative_accounts=" << report.negative_accounts << '\n'
              << "I3=" << (report.i3_final_status ? "PASS" : "FAIL")
              << " status_violations=" << report.status_violations << '\n'
              << "I4=" << (report.i4_no_pending ? "PASS" : "FAIL")
              << " pending=" << report.pending << '\n'
              << "I5=" << (report.i5_ledger_matches ? "PASS" : "FAIL")
              << " mismatches=" << report.ledger_mismatches << '\n';
    for (const std::string& violation : report.violations) {
        std::cerr << "consistency.error=" << violation << '\n';
    }
}

}  // namespace
}  // namespace bank

int main(int argc, char* argv[]) {
    try {
        const auto parsed_options = bank::parse_options(argc, argv);
        if (!parsed_options) {
            return 0;
        }
        const bank::SimulatorOptions options = *parsed_options;
        if (options.serve && (options.check || options.recover || !options.baseline_file.empty())) {
            throw std::invalid_argument("--serve cannot be combined with maintenance options");
        }
        if (options.check && options.baseline_file.empty()) {
            throw std::invalid_argument("--check requires --baseline-file");
        }
        if ((options.check || options.recover) && options.unsafe) {
            throw std::invalid_argument("--unsafe is only valid for simulation");
        }
        if (!options.check && !options.recover && !options.baseline_file.empty()) {
            throw std::invalid_argument("--baseline-file requires --check");
        }
        const bank::DbConfig config = bank::DbConfig::from_environment();
        if (options.serve) {
            bank::DashboardServer server(config, options.port);
            server.start();
            std::cout << "dashboard listening at http://127.0.0.1:" << server.port()
                      << "/\nPress Enter to stop and join the server gracefully.\n" << std::flush;
            std::string command;
            if (std::getline(std::cin, command)) { server.stop(); }
            else { server.wait(); }
            return 0;
        }
        if (options.check || options.recover) {
            bank::DbConnection connection(config);
            if (options.recover) {
                const bank::RecoveryReport recovery = bank::Recovery(connection).run();
                bank::print_recovery_report("recovery", recovery);
                if (!recovery.succeeded) {
                    return 1;
                }
            }
            if (options.check) {
                const std::map<int, bank::Money> baseline = bank::load_baseline(options.baseline_file);
                const bank::ConsistencyReport report = bank::ConsistencyChecker(connection).check(baseline);
                bank::print_consistency_report(report);
                return report.passed ? 0 : 1;
            }
            return 0;
        }
        {
            bank::DbConnection connection(config);
            const bank::RecoveryReport recovery = bank::Recovery(connection).run();
            bank::print_recovery_report("startup_recovery", recovery);
            if (!recovery.succeeded) {
                return 1;
            }
        }
        const bank::SimulationReport report = bank::Simulator(config, options).run();
        std::cout << "mode=" << (options.unsafe ? "unsafe" : "safe")
                  << " committed=" << report.committed
                  << " rolled_back=" << report.rolled_back
                  << " total=" << report.total
                  << " lock_waits=" << report.lock_waits;
        const double average_latency_ms = report.total == 0
            ? 0.0 : static_cast<double>(report.service_latency_ns) /
                static_cast<double>(report.total) / 1'000'000.0;
        std::cout << " avg_service_latency_ms=" << average_latency_ms << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
