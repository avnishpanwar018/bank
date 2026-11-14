#include "bank/db/connection_pool.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace bank {

std::ptrdiff_t ConnectionPool::checked_size(std::size_t size) {
    if (size == 0 || size > static_cast<std::size_t>(kSemaphoreMax)) {
        throw std::invalid_argument("connection pool size is outside semaphore range");
    }
    return static_cast<std::ptrdiff_t>(size);
}

ConnectionPool::ConnectionPool(DbConfig config, std::size_t size)
    : available_(checked_size(size)) {
    connections_.reserve(size);
    free_indices_.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
        connections_.push_back(std::make_unique<DbConnection>(config));
        free_indices_.push_back(index);
    }
}

ConnectionPool::Lease::Lease(ConnectionPool* pool, std::size_t index) noexcept
    : pool_(pool), index_(index) {}

ConnectionPool::Lease::~Lease() noexcept {
    release();
}

ConnectionPool::Lease::Lease(Lease&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)), index_(other.index_) {}

ConnectionPool::Lease& ConnectionPool::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = std::exchange(other.pool_, nullptr);
        index_ = other.index_;
    }
    return *this;
}

DbConnection& ConnectionPool::Lease::connection() const {
    if (pool_ == nullptr) {
        throw std::logic_error("connection lease is empty");
    }
    return *pool_->connections_[index_];
}

void ConnectionPool::Lease::release() noexcept {
    if (pool_ != nullptr) {
        pool_->release(index_);
        pool_ = nullptr;
    }
}

ConnectionPool::Lease ConnectionPool::acquire() {
    available_.acquire();
    std::lock_guard lock(mutex_);
    if (free_indices_.empty()) {
        available_.release();
        throw std::logic_error("connection pool semaphore invariant violated");
    }
    const std::size_t index = free_indices_.back();
    free_indices_.pop_back();
    leases_in_use_.fetch_add(1, std::memory_order_relaxed);
    return Lease(this, index);
}

void ConnectionPool::release(std::size_t index) noexcept {
    {
        std::lock_guard lock(mutex_);
        free_indices_.push_back(index);
        leases_in_use_.fetch_sub(1, std::memory_order_relaxed);
    }
    available_.release();
}

}  // namespace bank
