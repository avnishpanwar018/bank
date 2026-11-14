#include "bank/concurrency/thread_pool.hpp"

#include "bank/core/account_manager.hpp"

#include <stdexcept>
#include <string>
#include <chrono>
#include <utility>

namespace bank {

ThreadPool::ThreadPool(std::size_t worker_count, BoundedQueue<Request>& queue,
                       TransactionManager& transactions, OperationCounters& counters)
    : queue_(queue), transactions_(transactions), counters_(counters) {
    if (worker_count == 0) {
        throw std::invalid_argument("thread pool worker count must be positive");
    }
    workers_.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers_.emplace_back(&ThreadPool::worker_loop, this);
        }
    } catch (...) {
        queue_.close();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

ThreadPool::~ThreadPool() noexcept {
    shutdown();
}

std::future<Result> ThreadPool::submit(Request request) {
    std::future<Result> future = request.done.get_future();
    if (!queue_.push(std::move(request))) {
        request.done.set_value({TxStatus::RolledBack, "QUEUE_CLOSED", Money{}, 1});
    }
    return future;
}

void ThreadPool::shutdown() noexcept {
    std::call_once(shutdown_once_, [this] {
        queue_.close();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    });
}

void ThreadPool::worker_loop() {
    while (std::optional<Request> request = queue_.pop()) {
        counters_.active.fetch_add(1, std::memory_order_relaxed);
        const auto started = std::chrono::steady_clock::now();
        Result result;
        try {
            result = execute(*request);
        } catch (const std::exception& error) {
            result = {TxStatus::RolledBack, std::string("WORKER_ERROR: ") + error.what(), Money{}, 1};
        } catch (...) {
            result = {TxStatus::RolledBack, "WORKER_ERROR: unknown exception", Money{}, 1};
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started);
        counters_.active.fetch_sub(1, std::memory_order_relaxed);
        complete(*request, std::move(result), static_cast<std::uint64_t>(elapsed.count()));
    }
}

Result ThreadPool::execute(Request& request) {
    return transactions_.execute(request);
}

void ThreadPool::complete(Request& request, Result result, std::uint64_t latency_ns) noexcept {
    counters_.record_completion(result, latency_ns);
    try {
        request.done.set_value(std::move(result));
    } catch (...) {
        // A request with a consumed or abandoned promise cannot be completed again.
    }
}

}  // namespace bank
