#include "bank/tools/recovery.hpp"

#include <array>
#include <stdexcept>
#include <string_view>

namespace bank {
namespace {

MYSQL_BIND string_param(const std::string& value, unsigned long& length) {
    MYSQL_BIND bind{};
    length = static_cast<unsigned long>(value.size());
    bind.buffer_type = MYSQL_TYPE_STRING;
    bind.buffer = const_cast<char*>(value.data());
    bind.buffer_length = length;
    bind.length = &length;
    return bind;
}

MYSQL_BIND longlong_param(std::int64_t& value) {
    MYSQL_BIND bind{};
    bind.buffer_type = MYSQL_TYPE_LONGLONG;
    bind.buffer = &value;
    bind.buffer_length = sizeof(value);
    return bind;
}

struct PendingRow {
    std::int64_t log_id = 0;
    std::string request_id;
};

}  // namespace

Recovery::Recovery(DbConnection& connection) : connection_(connection) {}

RecoveryReport Recovery::run() {
    RecoveryReport report;
    try {
        connection_.start_transaction();
        auto pending_statement = connection_.prepare(
            "SELECT log_id, request_id FROM transaction_log "
            "WHERE status = 'PENDING' ORDER BY log_id FOR UPDATE");
        pending_statement.execute();
        std::int64_t log_id = 0;
        char request_id[64]{};
        unsigned long request_length = 0;
        bool request_null = false;
        MYSQL_BIND log_bind = longlong_param(log_id);
        MYSQL_BIND request_bind{};
        request_bind.buffer_type = MYSQL_TYPE_STRING;
        request_bind.buffer = request_id;
        request_bind.buffer_length = sizeof(request_id);
        request_bind.length = &request_length;
        request_bind.is_null = &request_null;
        std::array<MYSQL_BIND, 2> results{log_bind, request_bind};
        pending_statement.bind_results(results);
        pending_statement.store_result();
        std::vector<PendingRow> pending;
        while (pending_statement.fetch() != MYSQL_NO_DATA) {
            pending.push_back({log_id, std::string(request_id, request_length)});
        }
        report.pending_seen = pending.size();

        for (const PendingRow& row : pending) {
            auto transaction_statement = connection_.prepare(
                "SELECT status FROM transactions WHERE request_id = ? LIMIT 1");
            unsigned long request_length_param = 0;
            auto request_param = string_param(row.request_id, request_length_param);
            std::array<MYSQL_BIND, 1> params{request_param};
            transaction_statement.bind_params(params);
            transaction_statement.execute();
            char status[16]{};
            unsigned long status_length = 0;
            bool status_null = false;
            MYSQL_BIND status_bind{};
            status_bind.buffer_type = MYSQL_TYPE_STRING;
            status_bind.buffer = status;
            status_bind.buffer_length = sizeof(status);
            status_bind.length = &status_length;
            status_bind.is_null = &status_null;
            std::array<MYSQL_BIND, 1> status_results{status_bind};
            transaction_statement.bind_results(status_results);
            transaction_statement.store_result();
            const bool found = transaction_statement.fetch() != MYSQL_NO_DATA;
            const bool committed = found && std::string_view(status, status_length) == "COMMITTED";
            const TxStatus final_status = committed ? TxStatus::Committed : TxStatus::RolledBack;
            const std::string reason = committed ? "RECOVERY_COMMITTED_LEDGER"
                                                 : (found ? "RECOVERY_ROLLED_BACK_LEDGER"
                                                          : "RECOVERY_CRASH");

            auto update = connection_.prepare(
                "UPDATE transaction_log SET status = ?, reason = ? "
                "WHERE log_id = ? AND status = 'PENDING'");
            std::string status_text = committed ? "COMMITTED" : "ROLLED_BACK";
            unsigned long status_text_length = 0;
            unsigned long reason_length = 0;
            auto status_param = string_param(status_text, status_text_length);
            auto reason_param = string_param(reason, reason_length);
            std::int64_t update_id = row.log_id;
            auto id_param = longlong_param(update_id);
            std::array<MYSQL_BIND, 3> update_params{status_param, reason_param, id_param};
            update.bind_params(update_params);
            update.execute();
            if (mysql_stmt_affected_rows(update.native_handle()) != 1) {
                throw std::runtime_error("recovery log update affected no row");
            }
            report.entries.push_back({row.log_id, row.request_id, final_status, reason});
            if (committed) {
                ++report.committed;
            } else {
                ++report.rolled_back;
            }
        }
        connection_.commit();

        auto remaining_statement = connection_.prepare(
            "SELECT COUNT(*) FROM transaction_log WHERE status = 'PENDING'");
        remaining_statement.execute();
        std::int64_t remaining = 0;
        MYSQL_BIND remaining_bind = longlong_param(remaining);
        std::array<MYSQL_BIND, 1> remaining_results{remaining_bind};
        remaining_statement.bind_results(remaining_results);
        remaining_statement.store_result();
        if (remaining_statement.fetch() == MYSQL_NO_DATA) {
            throw std::runtime_error("recovery remaining-pending query returned no row");
        }
        report.remaining_pending = static_cast<std::size_t>(remaining);
        report.succeeded = report.remaining_pending == 0;
    } catch (const std::exception& error) {
        try {
            connection_.rollback();
        } catch (const std::exception& rollback_error) {
            report.errors.push_back(std::string("rollback: ") + rollback_error.what());
        }
        report.errors.push_back(error.what());
        report.succeeded = false;
    }
    return report;
}

}  // namespace bank
