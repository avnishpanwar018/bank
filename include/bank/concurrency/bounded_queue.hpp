#pragma once

#include <climits>
#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bank {

template <typename T>
class BoundedQueue final {
public:
    explicit BoundedQueue(std::size_t capacity, std::atomic<std::size_t>* observed_size = nullptr)
        : capacity_(capacity), empty_(checked_capacity(capacity)), full_(0),
          observed_size_(observed_size) {
        if (capacity == 0) {
            throw std::invalid_argument("bounded queue capacity must be positive");
        }
        static_assert(std::is_nothrow_move_constructible_v<T>,
                      "bounded queue values must be nothrow movable");
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    bool push(const T& value) {
        return push_value(value);
    }

    bool push(T&& value) {
        return push_value(std::move(value));
    }

    template <typename Value>
    bool push_value(Value&& value) {
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return false;
            }
            ++waiting_producers_;
        }

        empty_.acquire();
        {
            std::lock_guard lock(mutex_);
            --waiting_producers_;
            if (closed_) {
                empty_.release();
                return false;
            }
            values_.push_back(std::forward<Value>(value));
            publish_size();
        }
        full_.release();
        return true;
    }

    [[nodiscard]] std::optional<T> pop() {
        {
            std::lock_guard lock(mutex_);
            if (closed_ && values_.empty()) {
                return std::nullopt;
            }
            ++waiting_consumers_;
        }

        full_.acquire();
        std::optional<T> value;
        {
            std::lock_guard lock(mutex_);
            --waiting_consumers_;
            if (values_.empty()) {
                if (closed_) {
                    return std::nullopt;
                }
                throw std::logic_error("bounded queue semaphore invariant violated");
            }
            value.emplace(std::move(values_.front()));
            values_.pop_front();
            publish_size();
        }
        empty_.release();
        return value;
    }

    void close() noexcept {
        std::size_t consumers_to_wake = 0;
        std::size_t producers_to_wake = 0;
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
            consumers_to_wake = waiting_consumers_;
            producers_to_wake = waiting_producers_;
        }
        release(full_, consumers_to_wake);
        release(empty_, producers_to_wake);
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return values_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] bool is_closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

private:
    // Called only under mutex_; HTTP reads the optional atomic, never mutex_.
    void publish_size() noexcept {
        if (observed_size_ != nullptr) {
            observed_size_->store(values_.size(), std::memory_order_relaxed);
        }
    }

    static constexpr std::ptrdiff_t kSemaphoreMax = INT_MAX;

    static std::ptrdiff_t checked_capacity(std::size_t capacity) {
        if (capacity == 0 || capacity > static_cast<std::size_t>(kSemaphoreMax)) {
            throw std::invalid_argument("bounded queue capacity is outside semaphore range");
        }
        return static_cast<std::ptrdiff_t>(capacity);
    }

    static void release(std::counting_semaphore<kSemaphoreMax>& semaphore,
                        std::size_t count) noexcept {
        if (count > 0) {
            semaphore.release(static_cast<std::ptrdiff_t>(count));
        }
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::counting_semaphore<kSemaphoreMax> empty_;
    std::counting_semaphore<kSemaphoreMax> full_;
    std::deque<T> values_;
    std::size_t waiting_producers_ = 0;
    std::size_t waiting_consumers_ = 0;
    bool closed_ = false;
    std::atomic<std::size_t>* observed_size_;
};

}  // namespace bank
