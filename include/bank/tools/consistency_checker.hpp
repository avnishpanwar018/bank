#pragma once

#include "bank/db/db_connection.hpp"
#include "bank/money.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bank {

struct ConsistencyReport {
    bool passed = false;
    bool i1_total_conserved = false;
    bool i2_no_negative_balance = false;
    bool i3_final_status = false;
    bool i4_no_pending = false;
    bool i5_ledger_matches = false;
    bool baseline_available = false;
    std::int64_t actual_total_minor = 0;
    std::int64_t expected_total_minor = 0;
    std::size_t negative_accounts = 0;
    std::size_t pending = 0;
    std::size_t status_violations = 0;
    std::size_t ledger_mismatches = 0;
    std::vector<std::string> violations;
};

class ConsistencyChecker final {
public:
    explicit ConsistencyChecker(DbConnection& connection);

    [[nodiscard]] ConsistencyReport check(const std::map<int, Money>& baseline = {});

private:
    DbConnection& connection_;
};

}  // namespace bank
