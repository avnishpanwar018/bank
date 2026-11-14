#pragma once

#include "bank/db/db_connection.hpp"
#include "bank/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bank {

struct RecoveryEntry {
    std::int64_t log_id = 0;
    std::string request_id;
    TxStatus status = TxStatus::RolledBack;
    std::string reason;
};

struct RecoveryReport {
    bool succeeded = false;
    std::size_t pending_seen = 0;
    std::size_t committed = 0;
    std::size_t rolled_back = 0;
    std::size_t remaining_pending = 0;
    std::vector<RecoveryEntry> entries;
    std::vector<std::string> errors;
};

class Recovery final {
public:
    explicit Recovery(DbConnection& connection);

    [[nodiscard]] RecoveryReport run();

private:
    DbConnection& connection_;
};

}  // namespace bank
