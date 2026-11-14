#include "bank/sim/simulator.hpp"

#include "bank/concurrency/bounded_queue.hpp"
#include "bank/concurrency/counters.hpp"
#include "bank/concurrency/thread_pool.hpp"
#include "bank/core/lock_manager.hpp"
#include "bank/logging/txn_log.hpp"
#include "bank/db/connection_pool.hpp"
#include "bank/sim/monitor.hpp"

#include <climits>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bank {
namespace {

Request make_request(std::size_t client, std::size_t operation, std::size_t account_count,
                     std::mt19937_64& generator) {
    std::uniform_int_distribution<int> account_distribution(
        1, static_cast<int>(account_count));
    std::uniform_int_distribution<int> operation_distribution(0, 3);
    std::uniform_int_distribution<std::int64_t> amount_distribution(1, 10000);

    Request request;
    request.request_id = "sim-" + std::to_string(client) + "-" + std::to_string(operation);
    request.from = account_distribution(generator);
    request.to = account_distribution(generator);
    request.amount = Money::from_minor_units(amount_distribution(generator));
    switch (operation_distribution(generator)) {
    case 0:
        request.type = TxType::Deposit;
        break;
    case 1:
        request.type = TxType::Withdraw;
        break;
    case 2:
        request.type = TxType::Transfer;
        break;
    default:
        request.type = TxType::Balance;
        request.amount = Money{};
        break;
    }
    return request;
}

}  // namespace

Simulator::Simulator(DbConfig config, SimulatorOptions options)
    : config_(std::move(config)), options_(options) {
    if (options_.accounts == 0 || options_.accounts > static_cast<std::size_t>(INT_MAX) ||
        options_.clients == 0 || options_.operations_per_client == 0 || options_.workers == 0 ||
        options_.queue_capacity == 0) {
        throw std::invalid_argument("simulator counts and capacities must be positive");
    }
}

SimulationReport Simulator::run() {
    BoundedQueue<Request> queue(options_.queue_capacity);
    ConnectionPool connections(config_, options_.workers);
    OperationCounters counters;
    LockManager lock_manager(counters.lock_waits);
    TxnLog log(config_);
    TransactionManager transactions(connections, lock_manager, log, options_.unsafe);
    ThreadPool pool(options_.workers, queue, transactions, counters);
    Monitor monitor(queue, counters, std::cout);
    if (options_.verbose) {
        monitor.start();
    }
    std::vector<std::thread> clients;
    clients.reserve(options_.clients);

    for (std::size_t client = 0; client < options_.clients; ++client) {
        clients.emplace_back([&, client] {
            std::mt19937_64 generator(options_.seed + client * 0x9E3779B97F4A7C15ULL);
            std::vector<std::future<Result>> results;
            results.reserve(options_.operations_per_client);
            for (std::size_t operation = 0; operation < options_.operations_per_client;
                 ++operation) {
                Request request = make_request(client, operation, options_.accounts, generator);
                results.push_back(pool.submit(std::move(request)));
            }
            for (auto& result : results) {
                static_cast<void>(result.get());
            }
        });
    }
    for (auto& client : clients) {
        client.join();
    }
    pool.shutdown();
    monitor.stop();

    return {
        counters.committed.load(std::memory_order_relaxed),
        counters.rolled_back.load(std::memory_order_relaxed),
        counters.total.load(std::memory_order_relaxed),
        counters.lock_waits.load(std::memory_order_relaxed),
        counters.service_latency_ns.load(std::memory_order_relaxed)};
}

}  // namespace bank
