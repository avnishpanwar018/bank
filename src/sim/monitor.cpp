#include "bank/sim/monitor.hpp"

#include <iomanip>
#include <stdexcept>

namespace bank {

Monitor::Monitor(const BoundedQueue<Request>& queue, const OperationCounters& counters,
                 std::ostream& output, std::chrono::milliseconds interval)
    : queue_(queue), counters_(counters), output_(output), interval_(interval) {
    if (interval_.count() <= 0) {
        throw std::invalid_argument("monitor interval must be positive");
    }
}

Monitor::~Monitor() noexcept {
    stop();
}

void Monitor::start() {
    std::lock_guard lock(state_mutex_);
    if (started_) {
        return;
    }
    started_at_ = std::chrono::steady_clock::now();
    stop_requested_ = false;
    started_ = true;
    try {
        worker_ = std::thread(&Monitor::run, this);
    } catch (...) {
        started_ = false;
        throw;
    }
}

void Monitor::stop() noexcept {
    {
        std::lock_guard lock(state_mutex_);
        if (!started_) {
            return;
        }
        stop_requested_ = true;
    }
    state_changed_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard lock(state_mutex_);
    started_ = false;
}

void Monitor::run() {
    print_snapshot();
    std::unique_lock lock(state_mutex_);
    while (!state_changed_.wait_for(lock, interval_, [this] { return stop_requested_; })) {
        lock.unlock();
        print_snapshot();
        lock.lock();
    }
    lock.unlock();
    print_snapshot();
}

void Monitor::print_snapshot() const {
    const CounterSnapshot snapshot = counters_.snapshot();
    const double elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started_at_).count();
    const double throughput = elapsed_seconds > 0.0
        ? static_cast<double>(snapshot.total) / elapsed_seconds : 0.0;
    const double average_latency_ms = snapshot.total > 0
        ? static_cast<double>(snapshot.service_latency_ns) /
            static_cast<double>(snapshot.total) / 1'000'000.0
        : 0.0;

    output_ << "[monitor t=" << std::fixed << std::setprecision(2) << elapsed_seconds
            << "s] queue=" << queue_.size() << '/' << queue_.capacity()
            << " active=" << snapshot.active
            << " committed=" << snapshot.committed
            << " rolled_back=" << snapshot.rolled_back
            << " total=" << snapshot.total
            << " throughput=" << throughput << " tx/s"
            << " avg_service_latency=" << average_latency_ms << "ms"
            << " lock_waits_total=" << snapshot.lock_waits << '\n';
}

}  // namespace bank
