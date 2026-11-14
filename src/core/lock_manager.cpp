#include "bank/core/lock_manager.hpp"

#include <algorithm>
#include <utility>

namespace bank {

LockManager::ScopedLocks::ScopedLocks(std::vector<std::shared_ptr<std::mutex>> mutexes,
                                      std::atomic<std::uint64_t>* lock_waits)
    : mutexes_(std::move(mutexes)) {
    locks_.reserve(mutexes_.size());
    for (const auto& mutex : mutexes_) {
        locks_.emplace_back(*mutex, std::defer_lock);
        std::unique_lock<std::mutex>& lock = locks_.back();
        if (!lock.try_lock()) {
            if (lock_waits != nullptr) {
                lock_waits->fetch_add(1, std::memory_order_relaxed);
            }
            lock.lock();
        }
    }
}

LockManager::ScopedLocks LockManager::lock(std::initializer_list<int> account_ids) {
    return lock(std::vector<int>(account_ids));
}

LockManager::ScopedLocks LockManager::lock(std::vector<int> account_ids) {
    std::sort(account_ids.begin(), account_ids.end());
    account_ids.erase(std::unique(account_ids.begin(), account_ids.end()), account_ids.end());

    std::vector<std::shared_ptr<std::mutex>> mutexes;
    mutexes.reserve(account_ids.size());
    for (const int account_id : account_ids) {
        mutexes.push_back(mutex_for(account_id));
    }
    return ScopedLocks(std::move(mutexes), lock_waits_);
}

std::shared_ptr<std::mutex> LockManager::mutex_for(int account_id) {
    std::lock_guard registry_lock(registry_mutex_);
    auto [iterator, inserted] = account_mutexes_.try_emplace(
        account_id, std::make_shared<std::mutex>());
    static_cast<void>(inserted);
    return iterator->second;
}

}  // namespace bank
