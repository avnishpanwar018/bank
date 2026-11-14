#pragma once
 
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bank {

class LockManager final {
public:
    class ScopedLocks final {
    public:
        ~ScopedLocks() = default;

        ScopedLocks(const ScopedLocks&) = delete;
        ScopedLocks& operator=(const ScopedLocks&) = delete;
        ScopedLocks(ScopedLocks&&) noexcept = default;
        ScopedLocks& operator=(ScopedLocks&&) noexcept = default;

    private:
        friend class LockManager;
        ScopedLocks(std::vector<std::shared_ptr<std::mutex>> mutexes,
                    std::atomic<std::uint64_t>* lock_waits);

        std::vector<std::shared_ptr<std::mutex>> mutexes_;
        std::vector<std::unique_lock<std::mutex>> locks_;
    };

    LockManager() = default;
    explicit LockManager(std::atomic<std::uint64_t>& lock_waits) noexcept
        : lock_waits_(&lock_waits) {}

    [[nodiscard]] ScopedLocks lock(std::initializer_list<int> account_ids);
    [[nodiscard]] ScopedLocks lock(std::vector<int> account_ids);

private:
    [[nodiscard]] std::shared_ptr<std::mutex> mutex_for(int account_id);

    std::mutex registry_mutex_;
    std::unordered_map<int, std::shared_ptr<std::mutex>> account_mutexes_;
    std::atomic<std::uint64_t>* lock_waits_ = nullptr;
};

}  // namespace bank
