#pragma once

#include "bank/money.hpp"

#include <future>
#include <string>

namespace bank {

enum class TxStatus {
    Pending,
    Committed,
    RolledBack,
};

enum class TxType {
    Deposit,
    Withdraw,
    Transfer,
    Balance,
};

struct Result {
    TxStatus status = TxStatus::RolledBack;
    std::string reason;
    Money balance{};
    int attempts = 1;
};

struct Request {
    std::string request_id;
    TxType type = TxType::Balance;
    int from = 0;
    int to = 0;
    Money amount{};
    std::promise<Result> done;

    Request() = default;
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;
    Request(Request&&) noexcept = default;
    Request& operator=(Request&&) noexcept = default;
};

[[nodiscard]] inline bool is_success(const Result& result) noexcept {
    return result.status == TxStatus::Committed;
}

}  // namespace bank
