#pragma once

#include "bank/db/db_connection.hpp"
#include "bank/types.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace bank {

struct LogToken {
    std::int64_t log_id = 0;
};

class TxnLog final {
public:
    explicit TxnLog(DbConfig config);

    TxnLog(const TxnLog&) = delete;
    TxnLog& operator=(const TxnLog&) = delete;

    [[nodiscard]] std::optional<Result> find_final(const std::string& request_id);
    [[nodiscard]] LogToken write_pending(const std::string& request_id, int attempt,
                                         const std::string& thread_id);
    void update_attempt(LogToken token, int attempt);
    void resolve(LogToken token, TxStatus status, const std::string& reason, int attempt);

private:
    void resolve_on(DbConnection& connection, LogToken token, TxStatus status,
                    const std::string& reason, int attempt);

    DbConfig config_;
    DbConnection connection_;
    std::mutex mutex_;
};

}  // namespace bank
