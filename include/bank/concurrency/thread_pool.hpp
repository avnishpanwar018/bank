#pragma once

#include "bank/concurrency/bounded_queue.hpp"
#include "bank/concurrency/counters.hpp"
#include "bank/core/transaction_manager.hpp"
#include "bank/types.hpp"

#include <cstddef>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace bank {

class ThreadPool final {
public:
    ThreadPool(std::size_t worker_count, BoundedQueue<Request>& queue,
               TransactionManager& transactions, OperationCounters& counters);
    ~ThreadPool() noexcept;

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    [[nodiscard]] std::future<Result> submit(Request request);
    void shutdown() noexcept;

private:
    void worker_loop();
    void complete(Request& request, Result result, std::uint64_t latency_ns) noexcept;
    [[nodiscard]] Result execute(Request& request);

    BoundedQueue<Request>& queue_;
    TransactionManager& transactions_;
    OperationCounters& counters_;
    std::vector<std::thread> workers_;
    std::once_flag shutdown_once_;
};

}  // namespace bank
