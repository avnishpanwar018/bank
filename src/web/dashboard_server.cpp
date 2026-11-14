#include "bank/web/dashboard_server.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "bank/concurrency/bounded_queue.hpp"
#include "bank/concurrency/counters.hpp"
#include "bank/concurrency/thread_pool.hpp"
#include "bank/core/transaction_manager.hpp"
#include "bank/db/connection_pool.hpp"
#include "bank/logging/txn_log.hpp"
#include "bank/sim/monitor.hpp"
#include "bank/tools/consistency_checker.hpp"
#include "bank/tools/recovery.hpp"

#include <httplib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bank {
namespace {

using namespace std::chrono_literals;

enum class RunScenario {
    Mixed,
    Deposit,
    Opposing,
    Crash,
};

std::string scenario_text(RunScenario scenario) {
    switch (scenario) {
    case RunScenario::Mixed: return "mixed";
    case RunScenario::Deposit: return "deposit";
    case RunScenario::Opposing: return "opposing";
    case RunScenario::Crash: return "crash";
    }
    return "mixed";
}

struct RunOptions {
    std::size_t accounts = 50;
    std::size_t clients = 100;
    std::size_t operations = 100;
    std::size_t workers = 8;
    std::uint64_t seed = 42;
    bool unsafe = false;
    std::string isolation = "REPEATABLE_READ";
    double hotspot_ratio = 0.0;
    RunScenario scenario = RunScenario::Mixed;
    std::optional<std::int64_t> expected_start_balance_minor;
};

// A per-process nonce plus a monotonic sequence prevents replay on restarts.
std::string request_prefix() {
    static const std::string nonce = [] {
        std::random_device random;
        std::ostringstream value;
        value << std::hex << random() << random();
        return value.str().substr(0, 16);
    }();
    static std::atomic<std::uint64_t> sequence{0};
    std::ostringstream value;
    value << 'd' << nonce << '-' << std::hex << sequence.fetch_add(1, std::memory_order_relaxed);
    return value.str();
}

std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                escaped += "\\u00";
                constexpr char digits[] = "0123456789abcdef";
                escaped.push_back(digits[(static_cast<unsigned char>(character) >> 4U) & 0x0FU]);
                escaped.push_back(digits[static_cast<unsigned char>(character) & 0x0FU]);
            } else {
                escaped.push_back(character);
            }
        }
    }
    return escaped;
}

std::string json_string(std::string_view value) {
    return "\"" + json_escape(value) + "\"";
}

std::string tx_status_text(TxStatus status) {
    switch (status) {
    case TxStatus::Pending: return "PENDING";
    case TxStatus::Committed: return "COMMITTED";
    case TxStatus::RolledBack: return "ROLLED_BACK";
    }
    return "UNKNOWN";
}

std::string rollback_reason_text(RollbackReason reason) {
    switch (reason) {
    case RollbackReason::InsufficientFunds: return "INSUFFICIENT_FUNDS";
    case RollbackReason::AccountNotFound: return "ACCOUNT_NOT_FOUND";
    case RollbackReason::AccountNotActive: return "ACCOUNT_NOT_ACTIVE";
    case RollbackReason::InvalidAmount: return "INVALID_AMOUNT";
    case RollbackReason::SameAccount: return "SAME_ACCOUNT";
    case RollbackReason::DeadlockExhausted: return "DB_DEADLOCK_EXHAUSTED";
    case RollbackReason::DatabaseError: return "DB_ERROR";
    case RollbackReason::QueueClosed: return "QUEUE_CLOSED";
    case RollbackReason::WorkerError: return "WORKER_ERROR";
    case RollbackReason::Other: return "OTHER";
    case RollbackReason::Count: break;
    }
    return "OTHER";
}

class JsonObject final {
public:
    static JsonObject parse(std::string_view text) {
        JsonObject object(text);
        object.parse_object();
        return object;
    }

    [[nodiscard]] bool has(std::string_view key) const {
        return values_.contains(std::string(key));
    }

    [[nodiscard]] std::string string(std::string_view key) const {
        const auto iterator = values_.find(std::string(key));
        if (iterator == values_.end() || iterator->second.kind != ValueKind::String) {
            throw std::invalid_argument("missing or invalid JSON string: " + std::string(key));
        }
        return iterator->second.value;
    }

    [[nodiscard]] std::string text(std::string_view key) const {
        const auto iterator = values_.find(std::string(key));
        if (iterator == values_.end() ||
            (iterator->second.kind != ValueKind::String &&
             iterator->second.kind != ValueKind::Number)) {
            throw std::invalid_argument("missing or invalid JSON text: " + std::string(key));
        }
        return iterator->second.value;
    }

    [[nodiscard]] bool boolean(std::string_view key) const {
        const auto iterator = values_.find(std::string(key));
        if (iterator == values_.end() || iterator->second.kind != ValueKind::Boolean) {
            throw std::invalid_argument("missing or invalid JSON boolean: " + std::string(key));
        }
        return iterator->second.value == "true";
    }

    [[nodiscard]] std::uint64_t unsigned_integer(std::string_view key) const {
        const auto value = raw_number(key);
        std::uint64_t parsed = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error != std::errc{} || end != value.data() + value.size()) {
            throw std::invalid_argument("invalid JSON integer: " + std::string(key));
        }
        return parsed;
    }

    [[nodiscard]] double number(std::string_view key) const {
        const auto value = raw_number(key);
        std::size_t consumed = 0;
        double parsed = 0.0;
        try {
            parsed = std::stod(value, &consumed);
        } catch (const std::exception&) {
            throw std::invalid_argument("invalid JSON number: " + std::string(key));
        }
        if (consumed != value.size() || !std::isfinite(parsed)) {
            throw std::invalid_argument("invalid JSON number: " + std::string(key));
        }
        return parsed;
    }

private:
    enum class ValueKind { String, Number, Boolean, Null };
    struct Value {
        ValueKind kind = ValueKind::Null;
        std::string value;
    };

    explicit JsonObject(std::string_view text) : text_(text) {}

    void skip_space() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\n' ||
                text_[position_] == '\r' || text_[position_] == '\t')) {
            ++position_;
        }
    }

    void expect(char expected) {
        skip_space();
        if (position_ >= text_.size() || text_[position_] != expected) {
            throw std::invalid_argument("malformed JSON object");
        }
        ++position_;
    }

    std::string parse_string() {
        expect('"');
        std::string result;
        while (position_ < text_.size()) {
            const char character = text_[position_++];
            if (character == '"') {
                return result;
            }
            if (character != '\\') {
                if (static_cast<unsigned char>(character) < 0x20U) {
                    throw std::invalid_argument("control character in JSON string");
                }
                result.push_back(character);
                continue;
            }
            if (position_ >= text_.size()) {
                throw std::invalid_argument("unterminated JSON escape");
            }
            const char escaped = text_[position_++];
            switch (escaped) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            default: throw std::invalid_argument("unsupported JSON escape");
            }
        }
        throw std::invalid_argument("unterminated JSON string");
    }

    Value parse_value() {
        skip_space();
        if (position_ >= text_.size()) {
            throw std::invalid_argument("missing JSON value");
        }
        if (text_[position_] == '"') {
            return {ValueKind::String, parse_string()};
        }
        const std::size_t start = position_;
        while (position_ < text_.size() && text_[position_] != ',' &&
               text_[position_] != '}' && text_[position_] != ' ' && text_[position_] != '\n' &&
               text_[position_] != '\r' && text_[position_] != '\t') {
            ++position_;
        }
        const std::string token(text_.substr(start, position_ - start));
        if (token == "true" || token == "false") {
            return {ValueKind::Boolean, token};
        }
        if (token == "null") {
            return {ValueKind::Null, token};
        }
        static const std::regex number_pattern(R"(-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?)");
        if (!std::regex_match(token, number_pattern)) {
            throw std::invalid_argument("invalid JSON value");
        }
        return {ValueKind::Number, token};
    }

    void parse_object() {
        expect('{');
        skip_space();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            skip_space();
            if (position_ != text_.size()) {
                throw std::invalid_argument("trailing JSON data");
            }
            return;
        }
        while (true) {
            const std::string key = parse_string();
            expect(':');
            if (!values_.emplace(key, parse_value()).second) {
                throw std::invalid_argument("duplicate JSON key");
            }
            skip_space();
            if (position_ >= text_.size()) {
                throw std::invalid_argument("unterminated JSON object");
            }
            if (text_[position_] == '}') {
                ++position_;
                skip_space();
                if (position_ != text_.size()) {
                    throw std::invalid_argument("trailing JSON data");
                }
                return;
            }
            expect(',');
        }
    }

    [[nodiscard]] std::string raw_number(std::string_view key) const {
        const auto iterator = values_.find(std::string(key));
        if (iterator == values_.end() || iterator->second.kind != ValueKind::Number) {
            throw std::invalid_argument("missing or invalid JSON number: " + std::string(key));
        }
        return iterator->second.value;
    }

    std::string_view text_;
    std::size_t position_ = 0;
    std::unordered_map<std::string, Value> values_;
};

MYSQL_BIND integer_param(int& value) {
    MYSQL_BIND bind{};
    bind.buffer_type = MYSQL_TYPE_LONG;
    bind.buffer = &value;
    bind.buffer_length = sizeof(value);
    return bind;
}

MYSQL_BIND longlong_param(std::int64_t& value) {
    MYSQL_BIND bind{};
    bind.buffer_type = MYSQL_TYPE_LONGLONG;
    bind.buffer = &value;
    bind.buffer_length = sizeof(value);
    return bind;
}

MYSQL_BIND string_param(const std::string& value, unsigned long& length) {
    MYSQL_BIND bind{};
    length = static_cast<unsigned long>(value.size());
    bind.buffer_type = MYSQL_TYPE_STRING;
    bind.buffer = const_cast<char*>(value.data());
    bind.buffer_length = length;
    bind.length = &length;
    return bind;
}

std::string reason_json(const CounterSnapshot& snapshot) {
    std::string result = "{";
    for (std::size_t index = 0; index < kRollbackReasonCount; ++index) {
        if (index != 0) {
            result += ',';
        }
        result += json_string(rollback_reason_text(static_cast<RollbackReason>(index)));
        result += ':';
        result += std::to_string(snapshot.rolled_back_by_reason[index]);
    }
    result += '}';
    return result;
}

std::uint64_t percentile_latency_ns(const CounterSnapshot& snapshot, double percentile) {
    std::uint64_t samples = 0;
    for (const auto count : snapshot.latency_buckets) { samples += count; }
    const std::uint64_t target = static_cast<std::uint64_t>(
        std::ceil(static_cast<double>(samples) * percentile));
    if (target == 0) {
        return 0;
    }
    std::uint64_t cumulative = 0;
    for (std::size_t index = 0; index < kLatencyBucketCount; ++index) {
        cumulative += snapshot.latency_buckets[index];
        if (cumulative >= target) {
            if (index >= 63) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            return (std::uint64_t{1} << (index + 1)) - 1;
        }
    }
    return 0;
}

class DashboardRun final {
public:
    DashboardRun(DbConfig config, RunOptions options)
        : options_(options), queue_(512, &queue_depth_), connections_(config, options.workers),
          lock_manager_(counters_.lock_waits), log_(config),
          transactions_(connections_, lock_manager_, log_, options.unsafe,
              [this](FaultPoint point, DbConnection&, const Request&, int) {
                  if (options_.scenario == RunScenario::Crash &&
                      point == FaultPoint::AfterDebitBeforeCredit) {
                      fault_fired_.store(true, std::memory_order_relaxed);
                      throw std::runtime_error("DEMO_ABORT_AFTER_DEBIT");
                  }
              }) {
        // Dashboard session setup only, before any worker starts.
        const char* sql = options.isolation == "READ_COMMITTED"
            ? "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED"
            : options.isolation == "SERIALIZABLE"
                ? "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE"
                : "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ";
        std::vector<ConnectionPool::Lease> leases;
        for (std::size_t index = 0; index < options.workers; ++index) {
            leases.push_back(connections_.acquire());
            auto statement = leases.back().connection().prepare(sql);
            statement.execute();
        }
        leases.clear();
        pool_ = std::make_unique<ThreadPool>(options.workers, queue_, transactions_, counters_);
    }

    ~DashboardRun() noexcept {
        stop();
    }

    DashboardRun(const DashboardRun&) = delete;
    DashboardRun& operator=(const DashboardRun&) = delete;

    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            throw std::logic_error("dashboard simulation is already running");
        }
        accepting_.store(true, std::memory_order_release);
        started_at_ = std::chrono::steady_clock::now();
        try {
            manager_thread_ = std::thread(&DashboardRun::run_clients, this);
        } catch (...) {
            accepting_.store(false, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            throw;
        }
    }

    void stop() noexcept {
        std::call_once(stop_once_, [this] {
            accepting_.store(false, std::memory_order_release);
            stop_requested_.store(true, std::memory_order_release);
            queue_.close();
            if (manager_thread_.joinable()) { manager_thread_.join(); }
            if (pool_ != nullptr) { pool_->shutdown(); }
            running_.store(false, std::memory_order_release);
        });
    }

    void finish() {
        if (manager_thread_.joinable()) { manager_thread_.join(); }
    }

    [[nodiscard]] bool fault_fired() const noexcept {
        return fault_fired_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::string error() const {
        std::lock_guard lock(error_mutex_);
        return run_error_;
    }

    [[nodiscard]] bool stopped_early() const noexcept {
        return stop_requested_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool accepting() const noexcept {
        return accepting_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::future<Result> submit(Request request) {
        if (!accepting()) {
            throw std::logic_error("dashboard simulation is not accepting requests");
        }
        return pool_->submit(std::move(request));
    }

    [[nodiscard]] CounterSnapshot snapshot() const noexcept {
        return counters_.snapshot();
    }

    [[nodiscard]] std::size_t queue_depth() const {
        return queue_depth_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t pool_leases_in_use() const noexcept {
        return connections_.in_use();
    }

    [[nodiscard]] std::size_t worker_count() const noexcept {
        return options_.workers;
    }

    [[nodiscard]] bool unsafe() const noexcept {
        return options_.unsafe;
    }

    [[nodiscard]] std::chrono::steady_clock::time_point started_at() const noexcept {
        return started_at_;
    }

    [[nodiscard]] RunScenario scenario() const noexcept {
        return options_.scenario;
    }

    [[nodiscard]] std::optional<std::int64_t> expected_balance_minor() const noexcept {
        if (!options_.expected_start_balance_minor || options_.scenario != RunScenario::Deposit) {
            return std::nullopt;
        }
        const auto count = static_cast<std::int64_t>(options_.clients * options_.operations);
        return *options_.expected_start_balance_minor + count;
    }

private:
    static Request make_request(std::size_t client,
                                const RunOptions& options,
                                std::mt19937_64& generator) {
        Request request;
        request.request_id = request_prefix();
        if (options.scenario == RunScenario::Deposit) {
            request.type = TxType::Deposit;
            request.from = 1;
            request.amount = Money::from_minor_units(1);
            return request;
        }
        if (options.scenario == RunScenario::Opposing || options.scenario == RunScenario::Crash) {
            request.type = TxType::Transfer;
            request.from = static_cast<int>(client % 2U) + 1;
            request.to = request.from == 1 ? 2 : 1;
            request.amount = Money::from_minor_units(1);
            return request;
        }

        std::uniform_int_distribution<int> account_distribution(
            1, static_cast<int>(options.accounts));
        std::uniform_int_distribution<int> operation_distribution(0, 3);
        std::uniform_int_distribution<std::int64_t> amount_distribution(1, 10000);
        std::uniform_real_distribution<double> ratio_distribution(0.0, 1.0);
        const auto choose_account = [&] {
            if (options.hotspot_ratio > 0.0 && ratio_distribution(generator) < options.hotspot_ratio) {
                return 1 + static_cast<int>(generator() % std::min<std::size_t>(2, options.accounts));
            }
            return account_distribution(generator);
        };
        request.from = choose_account();
        request.to = choose_account();
        request.amount = Money::from_minor_units(amount_distribution(generator));
        switch (operation_distribution(generator)) {
        case 0: request.type = TxType::Deposit; break;
        case 1: request.type = TxType::Withdraw; break;
        case 2: request.type = TxType::Transfer; break;
        default: request.type = TxType::Balance; request.amount = Money{}; break;
        }
        return request;
    }

    void run_clients() {
        std::vector<std::jthread> clients;
        clients.reserve(options_.clients);
        try {
          for (std::size_t client = 0; client < options_.clients; ++client) {
            clients.emplace_back([this, client] {
              try {
                std::mt19937_64 generator(options_.seed + client * 0x9E3779B97F4A7C15ULL);
                std::vector<std::future<Result>> results;
                results.reserve(std::min<std::size_t>(options_.operations, 256));
                for (std::size_t operation = 0; operation < options_.operations; ++operation) {
                    if (stop_requested_.load(std::memory_order_acquire)) {
                        break;
                    }
                    Request request = make_request(client, options_, generator);
                    results.push_back(pool_->submit(std::move(request)));
                    if (results.size() >= 256) {
                        for (auto& result : results) { static_cast<void>(result.get()); }
                        results.clear();
                    }
                }
                for (auto& result : results) {
                    static_cast<void>(result.get());
                }
              } catch (const std::exception& error) {
                record_error(error.what());
                stop_requested_.store(true, std::memory_order_release);
                accepting_.store(false, std::memory_order_release);
                queue_.close();
              } catch (...) {
                record_error("unknown client failure");
                stop_requested_.store(true, std::memory_order_release);
                queue_.close();
              }
            });
          }
        } catch (const std::exception& error) {
            record_error(error.what());
            stop_requested_.store(true, std::memory_order_release);
            queue_.close();
        }
        for (auto& client : clients) {
            if (client.joinable()) {
                client.join();
            }
        }
        accepting_.store(false, std::memory_order_release);
        pool_->shutdown();
        running_.store(false, std::memory_order_release);
    }

    RunOptions options_;
    void record_error(std::string_view message) {
        std::lock_guard lock(error_mutex_);
        if (run_error_.empty()) { run_error_ = message; }
    }
    mutable std::mutex error_mutex_; // error paths only; not transaction hot path
    std::string run_error_;
    std::atomic<std::size_t> queue_depth_{0};
    BoundedQueue<Request> queue_;
    ConnectionPool connections_;
    OperationCounters counters_;
    LockManager lock_manager_;
    TxnLog log_;
    TransactionManager transactions_;
    std::unique_ptr<ThreadPool> pool_;
    std::thread manager_thread_;
    std::once_flag stop_once_;
    std::atomic<bool> fault_fired_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> accepting_{false};
    std::atomic<bool> stop_requested_{false};
    std::chrono::steady_clock::time_point started_at_{};
};

}  // namespace

namespace {

std::string error_json(std::string_view reason) {
    return "{\"error\":{\"reason\":" + json_string(reason) + "}}";
}

void set_json(httplib::Response& response, int status, const std::string& body) {
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_content(body, "application/json; charset=utf-8");
}

void set_error(httplib::Response& response, int status, std::string_view reason) {
    set_json(response, status, error_json(reason));
}

std::optional<std::int64_t> query_total_balance(const DbConfig& config) {
    DbConnection connection(config);
    auto statement = connection.prepare(
        "SELECT COALESCE(SUM(balance), CAST('0.00' AS DECIMAL(15,2))) FROM accounts");
    statement.execute();
    char total_text[64]{};
    unsigned long total_length = 0;
    bool total_null = false;
    MYSQL_BIND result{};
    result.buffer_type = MYSQL_TYPE_STRING;
    result.buffer = total_text;
    result.buffer_length = sizeof(total_text);
    result.length = &total_length;
    result.is_null = &total_null;
    std::array<MYSQL_BIND, 1> results{result};
    statement.bind_results(results);
    statement.store_result();
    if (statement.fetch() == MYSQL_NO_DATA || total_null) {
        return std::nullopt;
    }
    return Money::from_decimal(std::string_view(total_text, total_length)).minor_units();
}

std::optional<std::int64_t> query_account_balance(const DbConfig& config, int account_id) {
    DbConnection connection(config);
    auto statement = connection.prepare(
        "SELECT balance FROM accounts WHERE account_id = ?");
    auto id_param = integer_param(account_id);
    std::array<MYSQL_BIND, 1> params{id_param};
    statement.bind_params(params);
    statement.execute();
    char balance_text[64]{};
    unsigned long balance_length = 0;
    bool balance_null = false;
    MYSQL_BIND result{};
    result.buffer_type = MYSQL_TYPE_STRING;
    result.buffer = balance_text;
    result.buffer_length = sizeof(balance_text);
    result.length = &balance_length;
    result.is_null = &balance_null;
    std::array<MYSQL_BIND, 1> results{result};
    statement.bind_results(results);
    statement.store_result();
    if (statement.fetch() == MYSQL_NO_DATA || balance_null) {
        return std::nullopt;
    }
    return Money::from_decimal(std::string_view(balance_text, balance_length)).minor_units();
}

std::map<int, Money> load_baseline(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::invalid_argument("cannot open baseline file: " + path);
    }
    std::map<int, Money> baseline;
    int account_id = 0;
    std::int64_t minor_units = 0;
    while (input >> account_id >> minor_units) {
        if (account_id <= 0 || minor_units < 0 ||
            !baseline.emplace(account_id, Money::from_minor_units(minor_units)).second) {
            throw std::invalid_argument("invalid or duplicate baseline entry");
        }
    }
    if (!input.eof() || baseline.empty()) {
        throw std::invalid_argument("baseline file is empty or malformed");
    }
    return baseline;
}

std::string consistency_json(const ConsistencyReport& report) {
    std::string result = "{\"passed\":" + std::string(report.passed ? "true" : "false") +
                         ",\"I1\":" + std::string(report.i1_total_conserved ? "true" : "false") +
                         ",\"I2\":" + std::string(report.i2_no_negative_balance ? "true" : "false") +
                         ",\"I3\":" + std::string(report.i3_final_status ? "true" : "false") +
                         ",\"I4\":" + std::string(report.i4_no_pending ? "true" : "false") +
                         ",\"I5\":" + std::string(report.i5_ledger_matches ? "true" : "false") +
                         ",\"actual_total_minor\":" + std::to_string(report.actual_total_minor) +
                         ",\"expected_total_minor\":" + std::to_string(report.expected_total_minor) +
                         ",\"negative_accounts\":" + std::to_string(report.negative_accounts) +
                         ",\"pending\":" + std::to_string(report.pending) +
                         ",\"status_violations\":" + std::to_string(report.status_violations) +
                         ",\"ledger_mismatches\":" + std::to_string(report.ledger_mismatches) +
                         ",\"violations\":[";
    for (std::size_t index = 0; index < report.violations.size(); ++index) {
        if (index != 0) {
            result += ',';
        }
        result += json_string(report.violations[index]);
    }
    result += "]}";
    return result;
}

std::string recovery_json(const RecoveryReport& report) {
    std::string result = "{\"succeeded\":" + std::string(report.succeeded ? "true" : "false") +
                         ",\"pending_seen\":" + std::to_string(report.pending_seen) +
                         ",\"committed\":" + std::to_string(report.committed) +
                         ",\"rolled_back\":" + std::to_string(report.rolled_back) +
                         ",\"remaining_pending\":" + std::to_string(report.remaining_pending) +
                         ",\"errors\":[";
    for (std::size_t index = 0; index < report.errors.size(); ++index) {
        if (index != 0) {
            result += ',';
        }
        result += json_string(report.errors[index]);
    }
    result += "]}";
    return result;
}

std::string result_json(const Result& result) {
    return "{\"status\":" + json_string(tx_status_text(result.status)) +
           ",\"reason\":" + json_string(result.reason) +
           ",\"balance_minor\":" + std::to_string(result.balance.minor_units()) +
           ",\"attempts\":" + std::to_string(result.attempts) + "}";
}

std::string uppercase(std::string value) {
    for (char& character : value) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return value;
}

bool valid_status_filter(std::string_view value) {
    return value == "COMMITTED" || value == "ROLLED_BACK";
}

void delete_all(DbConnection& connection, const char* sql) {
    auto statement = connection.prepare(sql);
    statement.execute();
}

void reset_database(const DbConfig& config) {
    const char* allow = std::getenv("BANK_DASHBOARD_ALLOW_RESET");
    if (config.database != "bank_test" && (allow == nullptr || std::string_view(allow) != "1")) {
        throw std::invalid_argument("reset is development-only: use bank_test or explicitly set BANK_DASHBOARD_ALLOW_RESET=1");
    }
    DbConnection connection(config);
    try {
        delete_all(connection, "SET FOREIGN_KEY_CHECKS = 0");
        delete_all(connection, "TRUNCATE TABLE transaction_log");
        delete_all(connection, "TRUNCATE TABLE transactions");
        delete_all(connection, "TRUNCATE TABLE accounts");
        delete_all(connection, "TRUNCATE TABLE customers");
        delete_all(connection, "SET FOREIGN_KEY_CHECKS = 1");
        // TRUNCATE implicitly commits in MySQL; only reseeding is transactional.
        connection.start_transaction();

        auto customer_insert = connection.prepare(
            "INSERT INTO customers (name, email) VALUES (?, ?)");
        auto account_insert = connection.prepare(
            "INSERT INTO accounts (customer_id, balance) VALUES (?, CAST(? AS DECIMAL(15,2)))");
        for (int index = 1; index <= 50; ++index) {
            std::string name = "Dashboard Customer " + std::to_string(index);
            std::ostringstream email;
            email << "bank-dashboard-" << std::setw(2) << std::setfill('0') << index
                  << "@example.test";
            unsigned long name_length = 0;
            unsigned long email_length = 0;
            auto name_param = string_param(name, name_length);
            const std::string email_text = email.str();
            auto email_param = string_param(email_text, email_length);
            std::array<MYSQL_BIND, 2> customer_params{name_param, email_param};
            customer_insert.bind_params(customer_params);
            customer_insert.execute();
            std::int64_t customer_id = static_cast<std::int64_t>(
                mysql_stmt_insert_id(customer_insert.native_handle()));

            std::string amount = "100000.00";
            unsigned long amount_length = 0;
            auto customer_param = longlong_param(customer_id);
            auto amount_param = string_param(amount, amount_length);
            std::array<MYSQL_BIND, 2> account_params{customer_param, amount_param};
            account_insert.bind_params(account_params);
            account_insert.execute();
        }
        connection.commit();
    } catch (...) {
        const auto failure = std::current_exception();
        try { delete_all(connection, "SET FOREIGN_KEY_CHECKS = 1"); connection.rollback();
        } catch (const std::exception& rollback_error) {
            throw std::runtime_error(std::string("reset failed; cleanup: ") + rollback_error.what());
        }
        std::rethrow_exception(failure);
    }
}

}  // namespace

class DashboardServer::Impl final {
public:
    Impl(DbConfig config, std::uint16_t port, std::string web_root, std::string baseline_file)
        : config_(std::move(config)), requested_port_(port), web_root_(std::move(web_root)),
          baseline_file_(std::move(baseline_file)) {
        configure_routes();
    }

    ~Impl() noexcept {
        stop();
    }

    void start() {
        std::lock_guard lock(server_mutex_);
        if (started_) {
            return;
        }
        if (!server_.set_mount_point("/", web_root_)) {
            throw std::runtime_error("web root could not be mounted: " + web_root_);
        }
        const int bound = requested_port_ == 0 ? server_.bind_to_any_port("127.0.0.1")
            : (server_.bind_to_port("127.0.0.1", requested_port_) ? requested_port_ : -1);
        if (bound <= 0) {
            throw std::runtime_error("could not bind dashboard to 127.0.0.1");
        }
        bound_port_ = static_cast<std::uint16_t>(bound);
        {
            std::lock_guard stream_lock(stream_mutex_);
            stopping_ = false;
        }
        started_at_ = std::chrono::steady_clock::now();
        started_ = true;
        server_thread_ = std::thread([this] {
            server_.listen_after_bind();
            std::lock_guard lock(server_mutex_);
            started_ = false;
        });
    }

    void wait() {
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    void stop() noexcept {
        {
            std::lock_guard lock(stream_mutex_);
            stopping_ = true;
        }
        stream_changed_.notify_all();
        {
            std::lock_guard command_lock(command_mutex_);
            const auto run = current_run();
            if (run) { run->stop(); }
        }
        server_.stop();
        if (server_thread_.joinable() &&
            server_thread_.get_id() != std::this_thread::get_id()) {
            server_thread_.join();
        }
        std::lock_guard lock(server_mutex_);
        started_ = false;
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return bound_port_;
    }

    [[nodiscard]] bool is_running() const noexcept {
        std::lock_guard lock(server_mutex_);
        return started_;
    }

private:
    void configure_routes();
    void handle_stats(const httplib::Request&, httplib::Response&);
    void handle_stream(const httplib::Request&, httplib::Response&);
    void handle_accounts(const httplib::Request&, httplib::Response&);
    void handle_transactions(const httplib::Request&, httplib::Response&);
    void handle_simulate(const httplib::Request&, httplib::Response&);
    void handle_stop(const httplib::Request&, httplib::Response&);
    void handle_transaction(const httplib::Request&, httplib::Response&);
    void handle_check(const httplib::Request&, httplib::Response&);
    void handle_recover(const httplib::Request&, httplib::Response&);
    void handle_reset(const httplib::Request&, httplib::Response&);
    void handle_demo(const httplib::Request&, httplib::Response&);

    [[nodiscard]] std::shared_ptr<DashboardRun> current_run() const {
        std::lock_guard lock(run_mutex_);
        return run_;
    }

    [[nodiscard]] std::string stats_json() const;
    [[nodiscard]] std::optional<std::int64_t> total_balance_snapshot(std::uint64_t idle_version) const;
    [[nodiscard]] RunOptions parse_run_options(const JsonObject& object) const;
    [[nodiscard]] std::shared_ptr<DashboardRun> start_run(RunOptions options);
    void remember_check(std::string value);

    DbConfig config_;
    const std::uint16_t requested_port_;
    const std::string web_root_;
    const std::string baseline_file_;
    httplib::Server server_;
    mutable std::mutex server_mutex_;
    mutable std::mutex run_mutex_;
    // Control-plane lifecycle only; never acquired by account/queue workers.
    std::mutex command_mutex_;
    mutable std::mutex balance_mutex_; // guarded by: balance_mutex_, read-side only
    mutable std::optional<std::int64_t> balance_cache_;
    mutable std::string balance_error_;
    mutable std::chrono::steady_clock::time_point balance_sampled_{};
    mutable std::uint64_t balance_version_ = 0;
    std::mutex stream_mutex_;
    std::condition_variable stream_changed_;
    std::shared_ptr<DashboardRun> run_;
    std::string latest_check_ = "null";
    std::thread server_thread_;
    std::uint16_t bound_port_ = 0;
    std::chrono::steady_clock::time_point started_at_{};
    bool last_unsafe_ = false;
    bool started_ = false;
    bool stopping_ = false;
    std::atomic<unsigned> stream_clients_{0};
};

void DashboardServer::Impl::configure_routes() {
    server_.set_payload_max_length(16 * 1024);
    server_.set_default_headers({{"X-Content-Type-Options", "nosniff"},
        {"Referrer-Policy", "no-referrer"}});
    // Read the optional auth token from the environment (set via BANK_DASHBOARD_AUTH_TOKEN).
    // If not set, auth is disabled for backward compatibility.
    const char* auth_token = std::getenv("BANK_DASHBOARD_AUTH_TOKEN");
    server_.set_pre_routing_handler([this, auth_token](const httplib::Request& request, httplib::Response& response) {
        const auto host = request.get_header_value("Host");
        if (host != "127.0.0.1:" + std::to_string(bound_port_)) {
            set_error(response, 403, "use the 127.0.0.1 dashboard URL");
            return httplib::Server::HandlerResponse::Handled;
        }
        // Token authentication: if an auth token is configured, require it on all routes.
        if (auth_token) {
            const auto provided = request.get_header_value("X-Auth-Token");
            if (!provided.empty() && provided != auth_token) {
                set_error(response, 401, "invalid or missing authentication token");
                return httplib::Server::HandlerResponse::Handled;
            }
            if (provided.empty() && auth_token != nullptr) {
                set_error(response, 401, "missing authentication token");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server_.Get("/api/stats", [this](const httplib::Request& request, httplib::Response& response) {
        handle_stats(request, response);
    });
    server_.Get("/api/stream", [this](const httplib::Request& request, httplib::Response& response) {
        handle_stream(request, response);
    });
    server_.Get("/api/accounts", [this](const httplib::Request& request, httplib::Response& response) {
        handle_accounts(request, response);
    });
    server_.Get("/api/transactions", [this](const httplib::Request& request,
                                               httplib::Response& response) {
        handle_transactions(request, response);
    });
    using Handler = void (Impl::*)(const httplib::Request&, httplib::Response&);
    const auto post = [this](const char* path, Handler handler) {
        server_.Post(path, [this, handler](const httplib::Request& request, httplib::Response& response) {
          try {
            // JSON-only same-origin commands prevent cross-site form/CSRF writes.
            if (!request.get_header_value("Content-Type").starts_with("application/json")) {
                set_error(response, 400, "commands require application/json"); return;
            }
            const auto origin = request.get_header_value("Origin");
            const std::string expected = "http://127.0.0.1:" + std::to_string(bound_port_);
            if (!origin.empty() && origin != expected) {
                set_error(response, 403, "cross-origin commands are not allowed"); return;
            }
            static_cast<void>(JsonObject::parse(request.body));
            std::unique_lock command(command_mutex_, std::defer_lock);
            if (handler != &Impl::handle_transaction && !command.try_lock()) {
                set_error(response, 409, "another control command is in progress"); return;
            }
            {
                std::lock_guard lock(stream_mutex_);
                if (stopping_) { set_error(response, 503, "server is stopping"); return; }
            }
            (this->*handler)(request, response);
          } catch (const std::invalid_argument& error) { set_error(response, 400, error.what());
          } catch (const std::out_of_range& error) { set_error(response, 400, error.what());
          } catch (const std::exception& error) { set_error(response, 500, error.what()); }
        });
    };
    post("/api/simulate", &Impl::handle_simulate);
    post("/api/simulate/stop", &Impl::handle_stop);
    post("/api/tx", &Impl::handle_transaction);
    post("/api/check", &Impl::handle_check);
    post("/api/recover", &Impl::handle_recover);
    post("/api/reset", &Impl::handle_reset);
    post("/api/demo", &Impl::handle_demo);
}

void DashboardServer::Impl::handle_stats(const httplib::Request&, httplib::Response& response) {
    try {
        set_json(response, 200, stats_json());
    } catch (const std::exception& error) {
        set_error(response, 500, error.what());
    }
}

void DashboardServer::Impl::handle_stream(const httplib::Request&, httplib::Response& response) {
    if (stream_clients_.fetch_add(1) >= 2) {
        stream_clients_.fetch_sub(1);
        set_error(response, 429, "stream limit reached; poll /api/stats instead");
        return;
    }
    response.set_header("Cache-Control", "no-cache");
    response.set_header("Connection", "keep-alive");
    response.set_chunked_content_provider(
        "text/event-stream; charset=utf-8",
        [this](std::size_t, httplib::DataSink& sink) {
            while (true) {
                {
                    std::lock_guard lock(stream_mutex_);
                    if (stopping_) {
                        break;
                    }
                }
                const std::string event = "data: " + stats_json() + "\n\n";
                if (!sink.write(event.data(), event.size())) {
                    break;
                }
                std::unique_lock lock(stream_mutex_);
                if (stream_changed_.wait_for(lock, 500ms, [this] { return stopping_; })) {
                    break;
                }
            }
            sink.done();
            return true;
        }, [this](bool) { stream_clients_.fetch_sub(1); });
}

void DashboardServer::Impl::handle_accounts(const httplib::Request& request,
                                             httplib::Response& response) {
    try {
        std::size_t page = 1;
        std::size_t limit = 50;
        if (request.has_param("page")) {
            page = JsonObject::parse("{\"value\":" + request.get_param_value("page") + "}")
                .unsigned_integer("value");
        }
        if (request.has_param("limit")) {
            limit = JsonObject::parse("{\"value\":" + request.get_param_value("limit") + "}")
                .unsigned_integer("value");
        }
        if (page == 0 || limit == 0 || limit > 100) {
            throw std::invalid_argument("page must be positive and limit must be between 1 and 100");
        }
        if (page - 1 > static_cast<std::size_t>(std::numeric_limits<int>::max()) / limit) {
            throw std::invalid_argument("page is too large");
        }
        const std::size_t offset_value = (page - 1) * limit;
        int limit_value = static_cast<int>(limit);
        int offset = static_cast<int>(offset_value);
        DbConnection connection(config_);
        auto statement = connection.prepare(
            "SELECT a.account_id, c.name, a.balance, a.status "
            "FROM accounts a JOIN customers c ON c.customer_id = a.customer_id "
            "ORDER BY a.account_id LIMIT ? OFFSET ?");
        auto limit_param = integer_param(limit_value);
        auto offset_param = integer_param(offset);
        std::array<MYSQL_BIND, 2> params{limit_param, offset_param};
        statement.bind_params(params);
        statement.execute();

        int account_id = 0;
        char owner[128]{};
        unsigned long owner_length = 0;
        bool owner_null = false;
        char balance[64]{};
        unsigned long balance_length = 0;
        bool balance_null = false;
        char status[16]{};
        unsigned long status_length = 0;
        bool status_null = false;
        std::array<MYSQL_BIND, 4> results{};
        results[0] = integer_param(account_id);
        results[1].buffer_type = MYSQL_TYPE_STRING;
        results[1].buffer = owner;
        results[1].buffer_length = sizeof(owner);
        results[1].length = &owner_length;
        results[1].is_null = &owner_null;
        results[2].buffer_type = MYSQL_TYPE_STRING;
        results[2].buffer = balance;
        results[2].buffer_length = sizeof(balance);
        results[2].length = &balance_length;
        results[2].is_null = &balance_null;
        results[3].buffer_type = MYSQL_TYPE_STRING;
        results[3].buffer = status;
        results[3].buffer_length = sizeof(status);
        results[3].length = &status_length;
        results[3].is_null = &status_null;
        statement.bind_results(results);
        statement.store_result();

        std::string body = "{\"page\":" + std::to_string(page) +
                           ",\"limit\":" + std::to_string(limit) + ",\"accounts\":[";
        bool first = true;
        while (statement.fetch() != MYSQL_NO_DATA) {
            if (!first) {
                body += ',';
            }
            first = false;
            const std::string owner_value(owner, owner_length);
            const std::string balance_value(balance, balance_length);
            const std::string status_value(status, status_length);
            body += "{\"id\":" + std::to_string(account_id) +
                    ",\"owner\":" + json_string(owner_value) +
                    ",\"balance\":" + json_string(balance_value) +
                    ",\"balance_minor\":" +
                    std::to_string(Money::from_decimal(balance_value).minor_units()) +
                    ",\"status\":" + json_string(status_value) + "}";
        }
        body += "]}";
        set_json(response, 200, body);
    } catch (const std::invalid_argument& error) {
        set_error(response, 400, error.what());
    } catch (const std::exception& error) {
        set_error(response, 500, error.what());
    }
}

void DashboardServer::Impl::handle_transactions(const httplib::Request& request,
                                                  httplib::Response& response) {
    try {
        std::size_t limit = 50;
        if (request.has_param("limit")) {
            limit = JsonObject::parse("{\"value\":" + request.get_param_value("limit") + "}")
                .unsigned_integer("value");
        }
        if (limit == 0 || limit > 100) {
            throw std::invalid_argument("limit must be between 1 and 100");
        }
        const std::string filter = request.has_param("status")
            ? uppercase(request.get_param_value("status")) : "";
        if (!filter.empty() && !valid_status_filter(filter)) {
            throw std::invalid_argument("status must be COMMITTED or ROLLED_BACK");
        }
        int limit_value = static_cast<int>(limit);
        DbConnection connection(config_);
        const char* filtered_sql =
            "SELECT t.txn_id, t.request_id, t.type, COALESCE(t.from_account, 0), "
            "COALESCE(t.to_account, 0), t.amount, t.status, "
            "COALESCE((SELECT l.status FROM transaction_log l WHERE l.request_id = t.request_id "
            "ORDER BY l.log_id DESC LIMIT 1), t.status), "
            "DATE_FORMAT(t.created_at, '%Y-%m-%dT%H:%i:%s') "
            "FROM transactions t WHERE t.status = ? ORDER BY t.txn_id DESC LIMIT ?";
        const char* all_sql =
            "SELECT t.txn_id, t.request_id, t.type, COALESCE(t.from_account, 0), "
            "COALESCE(t.to_account, 0), t.amount, t.status, "
            "COALESCE((SELECT l.status FROM transaction_log l WHERE l.request_id = t.request_id "
            "ORDER BY l.log_id DESC LIMIT 1), t.status), "
            "DATE_FORMAT(t.created_at, '%Y-%m-%dT%H:%i:%s') "
            "FROM transactions t ORDER BY t.txn_id DESC LIMIT ?";
        auto statement = connection.prepare(filter.empty() ? all_sql : filtered_sql);
        unsigned long filter_length = 0;
        if (!filter.empty()) {
            auto filter_param = string_param(filter, filter_length);
            auto limit_param = integer_param(limit_value);
            std::array<MYSQL_BIND, 2> params{filter_param, limit_param};
            statement.bind_params(params);
        } else {
            auto limit_param = integer_param(limit_value);
            std::array<MYSQL_BIND, 1> params{limit_param};
            statement.bind_params(params);
        }
        statement.execute();

        std::int64_t txn_id = 0;
        char request_id[64]{};
        unsigned long request_length = 0;
        char type[16]{};
        unsigned long type_length = 0;
        int from = 0;
        int to = 0;
        char amount[64]{};
        unsigned long amount_length = 0;
        char status[16]{};
        unsigned long status_length = 0;
        char log_status[16]{};
        unsigned long log_status_length = 0;
        char created_at[32]{};
        unsigned long created_length = 0;
        std::array<MYSQL_BIND, 9> results{};
        results[0] = longlong_param(txn_id);
        for (auto* value : {request_id, type, amount, status, log_status, created_at}) {
            static_cast<void>(value);
        }
        results[1].buffer_type = MYSQL_TYPE_STRING;
        results[1].buffer = request_id;
        results[1].buffer_length = sizeof(request_id);
        results[1].length = &request_length;
        results[2].buffer_type = MYSQL_TYPE_STRING;
        results[2].buffer = type;
        results[2].buffer_length = sizeof(type);
        results[2].length = &type_length;
        results[3] = integer_param(from);
        results[4] = integer_param(to);
        results[5].buffer_type = MYSQL_TYPE_STRING;
        results[5].buffer = amount;
        results[5].buffer_length = sizeof(amount);
        results[5].length = &amount_length;
        results[6].buffer_type = MYSQL_TYPE_STRING;
        results[6].buffer = status;
        results[6].buffer_length = sizeof(status);
        results[6].length = &status_length;
        results[7].buffer_type = MYSQL_TYPE_STRING;
        results[7].buffer = log_status;
        results[7].buffer_length = sizeof(log_status);
        results[7].length = &log_status_length;
        results[8].buffer_type = MYSQL_TYPE_STRING;
        results[8].buffer = created_at;
        results[8].buffer_length = sizeof(created_at);
        results[8].length = &created_length;
        statement.bind_results(results);
        statement.store_result();

        std::string body = "{\"transactions\":[";
        bool first = true;
        while (statement.fetch() != MYSQL_NO_DATA) {
            if (!first) {
                body += ',';
            }
            first = false;
            body += "{\"id\":" + std::to_string(txn_id) +
                    ",\"request_id\":" + json_string(std::string(request_id, request_length)) +
                    ",\"type\":" + json_string(std::string(type, type_length)) +
                    ",\"from\":" + std::to_string(from) +
                    ",\"to\":" + std::to_string(to) +
                    ",\"amount\":" + json_string(std::string(amount, amount_length)) +
                    ",\"status\":" + json_string(std::string(status, status_length)) +
                    ",\"log_status\":" + json_string(std::string(log_status, log_status_length)) +
                    ",\"created_at\":" + json_string(std::string(created_at, created_length)) + "}";
        }
        body += "]}";
        set_json(response, 200, body);
    } catch (const std::invalid_argument& error) {
        set_error(response, 400, error.what());
    } catch (const std::exception& error) {
        set_error(response, 500, error.what());
    }
}

std::string DashboardServer::Impl::stats_json() const {
    const auto run = current_run();
    CounterSnapshot snapshot{};
    std::size_t queue_depth = 0;
    std::size_t pool_in_use = 0;
    std::size_t workers = 0;
    bool running = false;
    bool unsafe = false;
    std::string latest_check;
    std::string scenario = "mixed";
    std::optional<std::int64_t> expected_balance;
    std::string run_error;
    bool stopped_early = false;
    {
        std::lock_guard lock(run_mutex_);
        unsafe = last_unsafe_;
        latest_check = latest_check_;
    }
    std::chrono::steady_clock::time_point started;
    {
        std::lock_guard lock(server_mutex_);
        started = started_at_;
    }
    if (run) {
        snapshot = run->snapshot();
        queue_depth = run->queue_depth();
        pool_in_use = run->pool_leases_in_use();
        workers = run->worker_count();
        running = run->running();
        unsafe = run->unsafe();
        started = run->started_at();
        scenario = scenario_text(run->scenario());
        expected_balance = run->expected_balance_minor();
        run_error = run->error();
        stopped_early = run->stopped_early();
    }
    const double elapsed = std::max(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), 0.001);
    const double throughput = static_cast<double>(snapshot.total) / elapsed;
    const double average_latency_ms = snapshot.total == 0
        ? 0.0 : static_cast<double>(snapshot.service_latency_ns) /
            static_cast<double>(snapshot.total) / 1'000'000.0;
    const double p95_latency_ms = static_cast<double>(percentile_latency_ns(snapshot, 0.95)) /
                                  1'000'000.0;
    std::optional<std::int64_t> total_balance;
    try {
        total_balance = total_balance_snapshot(running ? std::numeric_limits<std::uint64_t>::max() : snapshot.total);
    } catch (...) {
        total_balance = std::nullopt;
    }
    std::optional<std::int64_t> actual_demo_balance;
    if (expected_balance) {
        try {
            actual_demo_balance = query_account_balance(config_, 1);
        } catch (...) {
            actual_demo_balance = std::nullopt;
        }
    }

    std::ostringstream numbers;
    numbers << std::fixed << std::setprecision(3)
            << throughput << '|' << average_latency_ms << '|' << p95_latency_ms;
    std::string throughput_text;
    std::string average_text;
    std::string p95_text;
    std::istringstream split(numbers.str());
    std::getline(split, throughput_text, '|');
    std::getline(split, average_text, '|');
    std::getline(split, p95_text, '|');

    std::string result = "{\"running\":" + std::string(running ? "true" : "false") +
                         ",\"mode\":" + json_string(unsafe ? "unsafe" : "safe") +
                         ",\"scenario\":" + json_string(scenario) +
                         ",\"uptime_s\":" + std::to_string(elapsed) +
                         ",\"committed\":" + std::to_string(snapshot.committed) +
                         ",\"rolled_back\":" + std::to_string(snapshot.rolled_back) +
                         ",\"retries\":" + std::to_string(snapshot.retries) +
                         ",\"throughput_tx_s\":" + throughput_text +
                         ",\"avg_latency_ms\":" + average_text +
                         ",\"p95_latency_ms\":" + p95_text +
                         ",\"queue_depth\":" + std::to_string(queue_depth) +
                         ",\"active_workers\":" + std::to_string(snapshot.active) +
                         ",\"workers\":" + std::to_string(workers) +
                         ",\"pool_leases_in_use\":" + std::to_string(pool_in_use) +
                         ",\"total_balance_minor\":";
    result += total_balance ? std::to_string(*total_balance) : "null";
    result += ",\"expected_demo_balance_minor\":";
    result += expected_balance ? std::to_string(*expected_balance) : "null";
    result += ",\"actual_demo_balance_minor\":";
    result += actual_demo_balance ? std::to_string(*actual_demo_balance) : "null";
    result += ",\"run_error\":" + json_string(run_error);
    result += ",\"demo_result\":";
    if (scenario == "deposit" || scenario == "opposing") {
        const std::string kind = scenario == "opposing" ? "DEADLOCK" : unsafe ? "RACE" : "SAFE";
        result += "{\"kind\":" + json_string(kind) + ",\"finished\":" + (running ? "false" : "true") +
            ",\"committed\":" + std::to_string(snapshot.committed) +
            ",\"rolled_back\":" + std::to_string(snapshot.rolled_back) + ",\"expected_minor\":" +
            (expected_balance ? std::to_string(*expected_balance) : "null") + ",\"actual_minor\":" +
            (actual_demo_balance ? std::to_string(*actual_demo_balance) : "null") + ",\"lost_minor\":" +
            (expected_balance && actual_demo_balance ? std::to_string(*expected_balance - *actual_demo_balance) : "null") +
            ",\"reason\":" + json_string(stopped_early ? "Run was stopped early; no correctness claim." : run_error) + "}";
    } else { result += "null"; }
    std::string balance_error;
    { std::lock_guard lock(balance_mutex_); balance_error = balance_error_; }
    result += ",\"database_error\":" + json_string(balance_error);
    result += ",\"rollback_reasons\":" + reason_json(snapshot) +
              ",\"lock_waits\":" + std::to_string(snapshot.lock_waits) +
              ",\"latest_check\":" + latest_check + "}";
    return result;
}

std::optional<std::int64_t> DashboardServer::Impl::total_balance_snapshot(std::uint64_t idle_version) const {
    std::lock_guard lock(balance_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (now - balance_sampled_ >= 500ms || (idle_version != balance_version_ &&
        idle_version != std::numeric_limits<std::uint64_t>::max())) {
        try { balance_cache_ = query_total_balance(config_); balance_error_.clear();
        } catch (const std::exception& error) {
            balance_cache_ = std::nullopt;
            balance_error_ = error.what();
        }
        balance_sampled_ = std::chrono::steady_clock::now();
        balance_version_ = idle_version;
    }
    return balance_cache_;
}

RunOptions DashboardServer::Impl::parse_run_options(const JsonObject& object) const {
    RunOptions options;
    options.accounts = object.unsigned_integer("accounts");
    options.clients = object.unsigned_integer("clients");
    options.operations = object.unsigned_integer("ops");
    options.workers = object.unsigned_integer("workers");
    options.seed = object.unsigned_integer("seed");
    options.unsafe = object.boolean("unsafe");
    options.hotspot_ratio = object.number("hotspot_ratio");
    options.isolation = uppercase(object.string("isolation"));
    if (options.isolation != "REPEATABLE_READ" && options.isolation != "READ_COMMITTED" &&
        options.isolation != "SERIALIZABLE") {
        throw std::invalid_argument("unsupported isolation level");
    }
    if (object.has("scenario")) {
        const std::string scenario = uppercase(object.string("scenario"));
        if (scenario == "RACE" || scenario == "DEPOSIT") {
            options.scenario = RunScenario::Deposit;
        } else if (scenario == "DEADLOCK" || scenario == "OPPOSING") {
            options.scenario = RunScenario::Opposing;
        } else if (scenario != "MIXED") {
            throw std::invalid_argument("scenario must be MIXED, RACE, or DEADLOCK");
        }
    }
    if (options.accounts == 0 || options.accounts > 500 || options.clients == 0 ||
        options.clients > 500 || options.operations == 0 || options.operations > 1'000'000 ||
        options.workers == 0 || options.workers > 64 || options.hotspot_ratio < 0.0 ||
        options.hotspot_ratio > 1.0) {
        throw std::invalid_argument("simulation values are outside safe dashboard limits");
    }
    if (options.scenario == RunScenario::Opposing && options.accounts < 2) {
        throw std::invalid_argument("deadlock demo requires at least two accounts");
    }
    if (options.operations > 5'000'000 / options.clients) {
        throw std::invalid_argument("total simulated operations exceed the dashboard cap");
    }
    return options;
}

std::shared_ptr<DashboardRun> DashboardServer::Impl::start_run(RunOptions options) {
    const auto previous = current_run();
    if (previous && previous->running()) {
        throw std::logic_error("a simulation is already running");
    }
    if (options.scenario == RunScenario::Deposit) {
        const auto starting_balance = query_account_balance(config_, 1);
        if (!starting_balance) {
            throw std::invalid_argument("race/safe demo requires account 1");
        }
        options.expected_start_balance_minor = *starting_balance;
    }
    auto run = std::make_shared<DashboardRun>(config_, options);
    run->start();
    {
        std::lock_guard lock(run_mutex_);
        run_ = run;
        last_unsafe_ = options.unsafe;
        latest_check_ = "null";
    }
    stream_changed_.notify_all();
    return run;
}

void DashboardServer::Impl::remember_check(std::string value) {
    std::lock_guard lock(run_mutex_);
    latest_check_ = std::move(value);
}

void DashboardServer::Impl::handle_simulate(const httplib::Request& request,
                                             httplib::Response& response) {
    try {
        const RunOptions options = parse_run_options(JsonObject::parse(request.body));
        static_cast<void>(start_run(options));
        set_json(response, 202, "{\"started\":true,\"mode\":" +
                               json_string(options.unsafe ? "unsafe" : "safe") + "}");
    } catch (const std::invalid_argument& error) {
        set_error(response, 400, error.what());
    } catch (const std::logic_error& error) {
        set_error(response, 409, error.what());
    } catch (const std::exception& error) {
        set_error(response, 500, error.what());
    }
}

void DashboardServer::Impl::handle_stop(const httplib::Request&, httplib::Response& response) {
    const auto run = current_run();
    if (!run || !run->running()) {
        set_error(response, 409, "no simulation is running");
        return;
    }
    run->stop();
    set_json(response, 200, "{\"stopped\":true}");
    stream_changed_.notify_all();
}

void DashboardServer::Impl::handle_transaction(const httplib::Request& request,
                                                httplib::Response& response) {
    try {
        const JsonObject object = JsonObject::parse(request.body);
        const std::string type_text = uppercase(object.string("type"));
        TxType type;
        if (type_text == "DEPOSIT") {
            type = TxType::Deposit;
        } else if (type_text == "WITHDRAW") {
            type = TxType::Withdraw;
        } else if (type_text == "TRANSFER") {
            type = TxType::Transfer;
        } else {
            throw std::invalid_argument("type must be DEPOSIT, WITHDRAW, or TRANSFER");
        }
        const std::uint64_t from_value = object.unsigned_integer("from");
        if (from_value == 0 || from_value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("from must be a positive account ID");
        }
        int from = static_cast<int>(from_value);
        int to = 0;
        if (type == TxType::Transfer) {
            const std::uint64_t to_value = object.unsigned_integer("to");
            if (to_value == 0 || to_value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                throw std::invalid_argument("to must be a positive account ID");
            }
            to = static_cast<int>(to_value);
        }
        const std::string amount_text = object.text("amount");
        const Money amount = Money::from_decimal(amount_text);
        if (!amount.is_positive() || amount.minor_units() > 999'999'999'999'999LL) {
            throw std::invalid_argument("amount must be positive and fit DECIMAL(15,2)");
        }
        Request transaction;
        transaction.request_id = request_prefix();
        transaction.type = type;
        transaction.from = from;
        transaction.to = to;
        transaction.amount = amount;
        std::future<Result> future;
        std::shared_ptr<DashboardRun> target;
        {
            target = current_run();
            if (!target || !target->accepting()) {
                set_error(response, 409, "start a simulation before submitting a transaction"); return;
            }
        }
        future = target->submit(std::move(transaction));
        const Result result = future.get();
        set_json(response, 200, result_json(result));
    } catch (const std::invalid_argument& error) {
        set_error(response, 400, error.what());
    } catch (const std::out_of_range& error) {
        set_error(response, 400, error.what());
    } catch (const std::logic_error& error) {
        set_error(response, 409, error.what());
    } catch (const std::exception& error) {
        set_error(response, 500, error.what());
    }
}

void DashboardServer::Impl::handle_check(const httplib::Request&, httplib::Response& response) {
    try {
        const auto run = current_run();
        if (run && run->running()) {
            set_error(response, 409, "stop the simulation before checking invariants");
            return;
        }
        DbConnection connection(config_);
        const ConsistencyReport report =
            ConsistencyChecker(connection).check(load_baseline(baseline_file_));
        const std::string body = consistency_json(report);
        remember_check(body);
        set_json(response, 200, body);
    } catch (const std::exception& error) {
        set_error(response, 500, error.what());
    }
}

void DashboardServer::Impl::handle_recover(const httplib::Request&, httplib::Response& response) {
    try {
        const auto run = current_run();
        if (run && run->running()) {
            set_error(response, 409, "stop the simulation before recovery");
            return;
        }
        DbConnection connection(config_);
        const RecoveryReport report = Recovery(connection).run();
        set_json(response, 200, recovery_json(report));
    } catch (const std::exception& error) {
        set_error(response, 500, error.what());
    }
}

void DashboardServer::Impl::handle_reset(const httplib::Request& request,
                                          httplib::Response& response) {
    try {
        const JsonObject object = JsonObject::parse(request.body);
        if (!object.has("confirm") || !object.boolean("confirm")) {
            throw std::invalid_argument("reset requires confirm=true");
        }
        const auto run = current_run();
        if (run && run->running()) {
            set_error(response, 409, "stop the simulation before resetting the database");
            return;
        }
        reset_database(config_);
        {
            std::lock_guard lock(balance_mutex_);
            balance_sampled_ = {};
        }
        remember_check("null");
        set_json(response, 200, "{\"reset\":true}");
    } catch (const std::invalid_argument& error) {
        set_error(response, 400, error.what());
    } catch (const std::exception& error) {
        set_error(response, 500, error.what());
    }
}

void DashboardServer::Impl::handle_demo(const httplib::Request& request,
                                         httplib::Response& response) {
    try {
        const std::string name = uppercase(JsonObject::parse(request.body).string("name"));
        if (name == "CRASH" || name == "CRASH_RECOVER") {
            RunOptions options;
            options.accounts = 2;
            options.clients = 1;
            options.operations = 1;
            options.workers = 1;
            options.scenario = RunScenario::Crash;
            auto crash = start_run(options);
            crash->finish();
            DbConnection connection(config_);
            const RecoveryReport report = Recovery(connection).run();
            const ConsistencyReport check = ConsistencyChecker(connection).check(load_baseline(baseline_file_));
            remember_check(consistency_json(check));
            const auto snapshot = crash->snapshot();
            const bool succeeded = crash->fault_fired() && snapshot.rolled_back == 1 && report.succeeded && check.passed;
            const std::string reason = "Injected exception after debit, before credit through the existing TransactionManager hook. InnoDB rolled back; recovery and I1-I5 then ran. This is not a process-kill durability test.";
            set_json(response, 200, "{\"succeeded\":" + std::string(succeeded ? "true" : "false") +
                ",\"fault_fired\":" + std::string(crash->fault_fired() ? "true" : "false") +
                ",\"reason\":" + json_string(reason) + ",\"recovery\":" + recovery_json(report) +
                ",\"check\":" + consistency_json(check) + "}");
            return;
        }
        RunOptions options;
        options.accounts = 2;
        options.clients = 20;
        options.operations = 5;
        options.workers = 8;
        options.seed = 42;
        options.unsafe = name == "RACE";
        options.scenario = name == "DEADLOCK" ? RunScenario::Opposing : RunScenario::Deposit;
        if (name != "RACE" && name != "SAFE" && name != "DEADLOCK") {
            throw std::invalid_argument("demo name must be RACE, SAFE, DEADLOCK, or CRASH_RECOVER");
        }
        static_cast<void>(start_run(options));
        set_json(response, 202, "{\"started\":true,\"demo\":" + json_string(name) + "}");
    } catch (const std::invalid_argument& error) {
        set_error(response, 400, error.what());
    } catch (const std::logic_error& error) {
        set_error(response, 409, error.what());
    } catch (const std::exception& error) {
        set_error(response, 500, error.what());
    }
}

DashboardServer::DashboardServer(DbConfig config, std::uint16_t port,
                                 std::string web_root, std::string baseline_file)
    : impl_(std::make_unique<Impl>(std::move(config), port, std::move(web_root),
                                   std::move(baseline_file))) {}

DashboardServer::~DashboardServer() noexcept = default;

void DashboardServer::start() {
    impl_->start();
}

void DashboardServer::wait() {
    impl_->wait();
}

void DashboardServer::stop() noexcept {
    impl_->stop();
}

std::uint16_t DashboardServer::port() const noexcept {
    return impl_->port();
}

bool DashboardServer::is_running() const noexcept {
    return impl_->is_running();
}

}  // namespace bank
