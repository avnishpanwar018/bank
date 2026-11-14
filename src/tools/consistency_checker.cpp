#include "bank/tools/consistency_checker.hpp"

#include "bank/money.hpp"

#include <array>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>

namespace bank {
namespace {

MYSQL_BIND int_param(int& value) {
    MYSQL_BIND bind{};
    bind.buffer_type = MYSQL_TYPE_LONG;
    bind.buffer = &value;
    bind.buffer_length = sizeof(value);
    return bind;
}

}  // namespace

ConsistencyChecker::ConsistencyChecker(DbConnection& connection) : connection_(connection) {}

ConsistencyReport ConsistencyChecker::check(const std::map<int, Money>& baseline) {
    ConsistencyReport report;
    report.baseline_available = !baseline.empty();
    std::map<int, Money> current;
    std::map<int, Money> replayed = baseline;
    std::map<std::string, int> final_statuses;
    std::int64_t net_delta = 0;

    try {
        connection_.start_transaction();
        auto account_statement = connection_.prepare(
            "SELECT account_id, balance FROM accounts ORDER BY account_id");
        account_statement.execute();
        int account_id = 0;
        char balance_text[64]{};
        unsigned long balance_length = 0;
        bool balance_null = false;
        MYSQL_BIND id_bind = int_param(account_id);
        MYSQL_BIND balance_bind{};
        balance_bind.buffer_type = MYSQL_TYPE_STRING;
        balance_bind.buffer = balance_text;
        balance_bind.buffer_length = sizeof(balance_text);
        balance_bind.length = &balance_length;
        balance_bind.is_null = &balance_null;
        std::array<MYSQL_BIND, 2> account_results{id_bind, balance_bind};
        account_statement.bind_results(account_results);
        account_statement.store_result();
        while (account_statement.fetch() != MYSQL_NO_DATA) {
            const Money balance = Money::from_decimal(std::string_view(balance_text, balance_length));
            current[account_id] = balance;
            report.actual_total_minor += balance.minor_units();
            if (balance.minor_units() < 0) {
                ++report.negative_accounts;
            }
        }
        report.i2_no_negative_balance = report.negative_accounts == 0;

        auto ledger_statement = connection_.prepare(
            "SELECT type, from_account, to_account, amount, status "
            "FROM transactions ORDER BY txn_id");
        ledger_statement.execute();
        char type[16]{};
        int from_account = 0;
        int to_account = 0;
        char amount_text[64]{};
        char status[16]{};
        unsigned long type_length = 0;
        unsigned long amount_length = 0;
        unsigned long status_length = 0;
        bool from_null = false;
        bool to_null = false;
        bool amount_null = false;
        bool status_null = false;
        MYSQL_BIND type_bind{};
        type_bind.buffer_type = MYSQL_TYPE_STRING;
        type_bind.buffer = type;
        type_bind.buffer_length = sizeof(type);
        type_bind.length = &type_length;
        MYSQL_BIND from_bind = int_param(from_account);
        from_bind.is_null = &from_null;
        MYSQL_BIND to_bind = int_param(to_account);
        to_bind.is_null = &to_null;
        MYSQL_BIND amount_bind{};
        amount_bind.buffer_type = MYSQL_TYPE_STRING;
        amount_bind.buffer = amount_text;
        amount_bind.buffer_length = sizeof(amount_text);
        amount_bind.length = &amount_length;
        amount_bind.is_null = &amount_null;
        MYSQL_BIND status_bind{};
        status_bind.buffer_type = MYSQL_TYPE_STRING;
        status_bind.buffer = status;
        status_bind.buffer_length = sizeof(status);
        status_bind.length = &status_length;
        status_bind.is_null = &status_null;
        std::array<MYSQL_BIND, 5> ledger_results{
            type_bind, from_bind, to_bind, amount_bind, status_bind};
        ledger_statement.bind_results(ledger_results);
        ledger_statement.store_result();
        while (ledger_statement.fetch() != MYSQL_NO_DATA) {
            const std::string type_value(type, type_length);
            const std::string status_value(status, status_length);
            const Money amount = Money::from_decimal(std::string_view(amount_text, amount_length));
            if (status_value != "COMMITTED") {
                continue;
            }
            if (type_value == "DEPOSIT" && !to_null) {
                replayed[to_account] = Money::from_minor_units(
                    replayed[to_account].minor_units() + amount.minor_units());
                net_delta += amount.minor_units();
            } else if (type_value == "WITHDRAW" && !from_null) {
                replayed[from_account] = Money::from_minor_units(
                    replayed[from_account].minor_units() - amount.minor_units());
                net_delta -= amount.minor_units();
            } else if (type_value == "TRANSFER" && !from_null && !to_null) {
                replayed[from_account] = Money::from_minor_units(
                    replayed[from_account].minor_units() - amount.minor_units());
                replayed[to_account] = Money::from_minor_units(
                    replayed[to_account].minor_units() + amount.minor_units());
            } else {
                ++report.ledger_mismatches;
            }
        }

        auto log_statement = connection_.prepare(
            "SELECT request_id, status FROM transaction_log ORDER BY request_id, log_id");
        log_statement.execute();
        char request_id[64]{};
        char log_status[16]{};
        unsigned long request_length = 0;
        unsigned long log_status_length = 0;
        MYSQL_BIND request_bind{};
        request_bind.buffer_type = MYSQL_TYPE_STRING;
        request_bind.buffer = request_id;
        request_bind.buffer_length = sizeof(request_id);
        request_bind.length = &request_length;
        MYSQL_BIND log_status_bind{};
        log_status_bind.buffer_type = MYSQL_TYPE_STRING;
        log_status_bind.buffer = log_status;
        log_status_bind.buffer_length = sizeof(log_status);
        log_status_bind.length = &log_status_length;
        std::array<MYSQL_BIND, 2> log_results{request_bind, log_status_bind};
        log_statement.bind_results(log_results);
        log_statement.store_result();
        while (log_statement.fetch() != MYSQL_NO_DATA) {
            const std::string request(request_id, request_length);
            const std::string status_value(log_status, log_status_length);
            if (status_value == "PENDING") {
                ++report.pending;
            } else {
                ++final_statuses[request];
            }
        }
        for (const auto& [request, count] : final_statuses) {
            static_cast<void>(request);
            if (count != 1) {
                ++report.status_violations;
            }
        }
        report.i4_no_pending = report.pending == 0;
        report.i3_final_status = report.status_violations == 0;

        if (report.baseline_available) {
            for (const auto& [id, balance] : current) {
                if (!baseline.contains(id) || !replayed.contains(id) || replayed[id] != balance) {
                    ++report.ledger_mismatches;
                }
            }
            for (const auto& [id, opening] : baseline) {
                static_cast<void>(opening);
                if (!current.contains(id)) {
                    ++report.ledger_mismatches;
                }
            }
            report.expected_total_minor = 0;
            for (const auto& [id, opening] : baseline) {
                static_cast<void>(id);
                report.expected_total_minor += opening.minor_units();
            }
            report.expected_total_minor += net_delta;
            report.i1_total_conserved = report.actual_total_minor == report.expected_total_minor;
            report.i5_ledger_matches = report.ledger_mismatches == 0;
        }
        connection_.commit();
        report.passed = report.i2_no_negative_balance && report.i3_final_status &&
                        report.i4_no_pending && report.baseline_available &&
                        report.i1_total_conserved && report.i5_ledger_matches;
    } catch (const std::exception& error) {
        try {
            connection_.rollback();
        } catch (const std::exception& rollback_error) {
            report.violations.push_back(std::string("rollback: ") + rollback_error.what());
        } catch (...) {
            report.violations.push_back("rollback: unknown exception");
        }
        report.violations.push_back(error.what());
        report.passed = false;
    }
    return report;
}

}  // namespace bank
