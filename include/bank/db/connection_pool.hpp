#pragma once

#include "bank/db/db_connection.hpp"

#include <climits>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <semaphore>
#include <vector>

namespace bank {

class ConnectionPool final {
public:
    class Lease final {
    public:
        Lease() noexcept = default;
        ~Lease() noexcept;

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        [[nodiscard]] DbConnection& connection() const;
        [[nodiscard]] explicit operator bool() const noexcept { return pool_ != nullptr; }

    private:
        friend class ConnectionPool;
        Lease(ConnectionPool* pool, std::size_t index) noexcept;
        void release() noexcept;

        ConnectionPool* pool_ = nullptr;
        std::size_t index_ = 0;
    };

    ConnectionPool(DbConfig config, std::size_t size);
    ~ConnectionPool() = default;

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    [[nodiscard]] Lease acquire();
    [[nodiscard]] std::size_t size() const noexcept { return connections_.size(); }
    [[nodiscard]] std::size_t in_use() const noexcept {
        return leases_in_use_.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::ptrdiff_t kSemaphoreMax = INT_MAX;

    static std::ptrdiff_t checked_size(std::size_t size);
    void release(std::size_t index) noexcept;

    std::vector<std::unique_ptr<DbConnection>> connections_;
    std::vector<std::size_t> free_indices_;
    mutable std::mutex mutex_;
    std::counting_semaphore<kSemaphoreMax> available_;
    std::atomic<std::size_t> leases_in_use_{0};
};

}  // namespace bank
