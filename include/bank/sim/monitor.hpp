#pragma once

#include "bank/concurrency/bounded_queue.hpp"
#include "bank/concurrency/counters.hpp"
#include "bank/types.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <ostream>
#include <thread>

namespace bank {

class Monitor final {
public:
    Monitor(const BoundedQueue<Request>& queue, const OperationCounters& counters,
            std::ostream& output,
            std::chrono::milliseconds interval = std::chrono::milliseconds{1000});
    ~Monitor() noexcept;

    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    void start();
    void stop() noexcept;

private:
    void run();
    void print_snapshot() const;

    const BoundedQueue<Request>& queue_;
    const OperationCounters& counters_;
    std::ostream& output_;
    const std::chrono::milliseconds interval_;
    std::mutex state_mutex_;
    std::condition_variable state_changed_;
    std::thread worker_;
    std::chrono::steady_clock::time_point started_at_{};
    bool started_ = false;
    bool stop_requested_ = false;
};

}  // namespace bank
