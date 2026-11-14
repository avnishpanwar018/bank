#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "bank/config.hpp"
#include "bank/core/account_manager.hpp"
#include "bank/db/db_connection.hpp"
#include "bank/logging/txn_log.hpp"
#include "bank/tools/consistency_checker.hpp"
#include "bank/web/dashboard_server.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <latch>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bank {
namespace {

constexpr std::int64_t kOpeningBalanceMinor = 10'000'000;
constexpr std::int64_t kOpeningTotalMinor = 50 * kOpeningBalanceMinor;

void configure_client(httplib::Client& client) {
    client.set_connection_timeout(5, 0);
    // Stop/manual requests can wait for the bounded queue and DB pool to drain.
    client.set_read_timeout(120, 0);
    client.set_write_timeout(120, 0);
}

class LatchRelease final {
public:
    explicit LatchRelease(std::latch& latch) : latch_(latch) {}
    ~LatchRelease() { release(); }

    void release() noexcept {
        if (!released_) {
            latch_.count_down();
            released_ = true;
        }
    }

private:
    std::latch& latch_;
    bool released_ = false;
};

// These helpers inspect the API's scalar fields and flat account/ledger arrays;
// they intentionally do not depend on JSON whitespace or object-key ordering.
std::string json_field(const std::string& body, const std::string& key) {
    const std::regex pattern("\"" + key +
        R"json("\s*:\s*("(?:[^"\\]|\\.)*"|true|false|null|-?[0-9]+(?:\.[0-9]+)?)(?=\s*[,}\]]))json");
    std::smatch match;
    if (!std::regex_search(body, match, pattern)) {
        throw std::runtime_error("missing/invalid JSON field " + key + ": " + body);
    }
    return match[1].str();
}

std::int64_t integer_field(const std::string& body, const std::string& key) {
    const std::string token = json_field(body, key);
    std::size_t consumed = 0;
    const auto value = std::stoll(token, &consumed);
    if (consumed != token.size()) {
        throw std::runtime_error("expected integer field: " + key);
    }
    return value;
}

std::vector<std::string> json_rows(const std::string& body, const std::string& key) {
    const std::regex array_pattern("\"" + key + R"("\s*:\s*\[([^\]]*)\])");
    std::smatch array_match;
    if (!std::regex_search(body, array_match, array_pattern)) {
        throw std::runtime_error("missing JSON array " + key + ": " + body);
    }
    const std::string contents = array_match[1].str();
    const std::regex row_pattern(R"(\{[^{}]*\})");
    std::vector<std::string> rows;
    for (std::sregex_iterator row(contents.begin(), contents.end(), row_pattern), end;
         row != end; ++row) {
        rows.push_back(row->str());
    }
    return rows;
}

std::string require_json(const httplib::Result& response, int expected_status = 200) {
    if (!response) {
        throw std::runtime_error("HTTP request failed: " + httplib::to_string(response.error()));
    }
    if (response->status != expected_status) {
        throw std::runtime_error("expected HTTP " + std::to_string(expected_status) +
            ", got " + std::to_string(response->status) + ": " + response->body);
    }
    if (!response->get_header_value("Content-Type").starts_with("application/json")) {
        throw std::runtime_error("expected JSON response: " + response->body);
    }
    return response->body;
}

void expect_error(const httplib::Result& response, int expected_status) {
    const std::string body = require_json(response, expected_status);
    EXPECT_NE(body.find("\"error\""), std::string::npos) << body;
    const std::string reason = json_field(body, "reason");
    EXPECT_TRUE(reason.starts_with('"')) << body;
    EXPECT_GT(reason.size(), 2U) << body;
}

using JsonOverrides = std::initializer_list<std::pair<std::string, std::string>>;

std::string simulation_body(JsonOverrides overrides = {}) {
    std::map<std::string, std::string> fields{
        {"accounts", "50"}, {"clients", "1"}, {"ops", "4"}, {"workers", "1"},
        {"seed", "42"}, {"unsafe", "false"}, {"isolation", R"("REPEATABLE_READ")"},
        {"hotspot_ratio", "0"}, {"scenario", R"("MIXED")"}};
    for (const auto& [key, value] : overrides) {
        fields[key] = value;
    }
    std::string body = "{";
    for (const auto& [key, value] : fields) {
        if (body.size() > 1) {
            body += ',';
        }
        body += '"' + key + "\":" + value;
    }
    return body + '}';
}

template <typename Predicate>
bool poll_stats(httplib::Client& client, Predicate predicate, std::string& last_stats) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    for (int poll = 0; poll < 512 && std::chrono::steady_clock::now() < deadline; ++poll) {
        last_stats = require_json(client.Get("/api/stats"));
        if (predicate(last_stats)) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

void expect_idle_stats(const std::string& stats) {
    EXPECT_EQ(json_field(stats, "running"), "false");
    EXPECT_EQ(integer_field(stats, "queue_depth"), 0);
    EXPECT_EQ(integer_field(stats, "active_workers"), 0);
    EXPECT_EQ(integer_field(stats, "pool_leases_in_use"), 0);
}

void expect_invariants(const std::string& report) {
    for (const char* field : {"passed", "I1", "I2", "I3", "I4", "I5"}) {
        EXPECT_EQ(json_field(report, field), "true") << field << ": " << report;
    }
    for (const char* field : {"negative_accounts", "pending", "status_violations",
                              "ledger_mismatches"}) {
        EXPECT_EQ(integer_field(report, field), 0) << field << ": " << report;
    }
    EXPECT_EQ(integer_field(report, "actual_total_minor"),
              integer_field(report, "expected_total_minor"));
}

class DashboardHttpTest : public ::testing::Test {
protected:
    void SetUp() override {
        DbConfig config;
        config.database = "bank_test";
        // The static/validation suite is independent of configured credentials.
        // A closed local port also prevents stats from consulting a real DB.
        config.port = 1;
        server = std::make_unique<DashboardServer>(config, 0);
        server->start();
        client = std::make_unique<httplib::Client>("127.0.0.1", server->port());
        configure_client(*client);
    }

    void TearDown() override {
        if (server) {
            server->stop();
        }
    }

    std::unique_ptr<DashboardServer> server;
    std::unique_ptr<httplib::Client> client;
};

TEST_F(DashboardHttpTest, ServesFrontendLocalAssetsAndIdleStatsWithoutDatabase) {
    const std::array<std::pair<const char*, const char*>, 4> assets{{
        {"/", "text/html"}, {"/app.js", "javascript"},
        {"/style.css", "text/css"}, {"/vendor/chart.umd.min.js", "javascript"}}};
    for (const auto& [path, content_type] : assets) {
        SCOPED_TRACE(path);
        const auto response = client->Get(path);
        ASSERT_TRUE(response) << httplib::to_string(response.error());
        EXPECT_EQ(response->status, 200);
        EXPECT_NE(response->get_header_value("Content-Type").find(content_type), std::string::npos);
        EXPECT_FALSE(response->body.empty());
        if (std::string_view(path) == "/") {
            EXPECT_NE(response->body.find("BANK OPS"), std::string::npos);
        }
    }
    const auto missing = client->Get("/missing-dashboard-asset.js");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);

    const std::string stats = require_json(client->Get("/api/stats"));
    expect_idle_stats(stats);
    EXPECT_EQ(json_field(stats, "mode"), R"("safe")");
    EXPECT_EQ(json_field(stats, "scenario"), R"("mixed")");
    for (const char* field : {"committed", "rolled_back", "retries", "workers", "lock_waits"}) {
        EXPECT_EQ(integer_field(stats, field), 0) << field;
    }
    for (const char* field : {"uptime_s", "throughput_tx_s", "avg_latency_ms", "p95_latency_ms"}) {
        EXPECT_GE(std::stod(json_field(stats, field)), 0.0) << field;
    }
    EXPECT_EQ(json_field(stats, "total_balance_minor"), "null");
    EXPECT_EQ(json_field(stats, "latest_check"), "null");
    EXPECT_EQ(integer_field(stats, "INSUFFICIENT_FUNDS"), 0);
}

TEST_F(DashboardHttpTest, StreamsFirstCompleteStatsEventAndAllowsClientCancellation) {
    int status = 0;
    std::string content_type;
    std::string cache_control;
    std::string stream;
    bool received_event = false;
    const auto response = client->Get("/api/stream",
        [&](const httplib::Response& headers) {
            status = headers.status;
            content_type = headers.get_header_value("Content-Type");
            cache_control = headers.get_header_value("Cache-Control");
            return true;
        },
        [&](const char* data, std::size_t length) {
            stream.append(data, length);
            received_event = stream.find("data: ") != std::string::npos &&
                             stream.find("\n\n") != std::string::npos;
            // SSE is unbounded: cancel immediately after the first data event.
            return !received_event && stream.size() < 64 * 1024;
        });
    EXPECT_EQ(status, 200);
    EXPECT_TRUE(content_type.starts_with("text/event-stream"));
    EXPECT_EQ(cache_control, "no-cache");
    ASSERT_TRUE(received_event) << stream;
    const auto begin = stream.find("data: ") + 6;
    const std::string event = stream.substr(begin, stream.find("\n\n", begin) - begin);
    expect_idle_stats(event);
    EXPECT_EQ(json_field(event, "mode"), R"("safe")");
    // cpp-httplib reports a receiver-requested early exit as Canceled.
    EXPECT_TRUE(response || response.error() == httplib::Error::Canceled)
        << httplib::to_string(response.error());
    EXPECT_EQ(json_field(require_json(client->Get("/api/stats")), "running"), "false");
}

TEST_F(DashboardHttpTest, RejectsInvalidAccountPaginationBeforeDatabaseAccess) {
    for (const char* query : {
             "page=0", "page=-1", "page=1.5", "page=abc", "page=", "page=true",
             "page=%221%22", "page=2147483649&limit=1",
             "page=18446744073709551616", "page=9223372036854775809&limit=2",
             "limit=0", "limit=-1", "limit=101", "limit=1.5", "limit=abc",
             "limit=", "limit=null", "limit=%2210%22"}) {
        SCOPED_TRACE(query);
        expect_error(client->Get(std::string("/api/accounts?") + query), 400);
    }
}

TEST_F(DashboardHttpTest, RejectsInvalidTransactionLimitsAndStatusBeforeDatabaseAccess) {
    for (const char* query : {
             "limit=0", "limit=-1", "limit=101", "limit=1.5", "limit=abc", "limit=",
             "limit=true", "limit=%2210%22", "limit=18446744073709551616",
             "status=PENDING", "status=UNKNOWN", "status=COMMITTED%27%20OR%201=1"}) {
        SCOPED_TRACE(query);
        expect_error(client->Get(std::string("/api/transactions?") + query), 400);
    }
}

TEST_F(DashboardHttpTest, RejectsSimulationBadJsonTypesRangesAndUnsupportedOptions) {
    for (const char* body : {"", "{", "[]", "null", "{}", "{} trailing"}) {
        SCOPED_TRACE(body);
        expect_error(client->Post("/api/simulate", body, "application/json"), 400);
    }
    const std::vector<std::pair<std::string, std::vector<std::string>>> invalid_fields{
        {"accounts", {"0", "501", "-1", "1.5", R"("50")", "true", "null"}},
        {"clients", {"0", "501", "-1", "1.5", R"("1")", "false", "null"}},
        {"ops", {"0", "1000001", "-1", "1.5", R"("4")", "true", "null"}},
        {"workers", {"0", "65", "-1", "1.5", R"("1")", "true", "null"}},
        {"seed", {"-1", "1.5", R"("42")", "true", "null", "18446744073709551616"}},
        {"unsafe", {"0", "1", R"("false")", "null"}},
        {"hotspot_ratio", {"-0.01", "1.01", R"("0.5")", "true", "null", "1e999"}},
        {"isolation", {R"("UNKNOWN")", R"("READ_UNCOMMITTED")", "1", "true", "null"}},
        {"scenario", {R"("UNKNOWN")", "1", "true", "null"}}};
    for (const auto& [field, values] : invalid_fields) {
        for (const auto& value : values) {
            SCOPED_TRACE(field + "=" + value);
            expect_error(client->Post("/api/simulate", simulation_body({{field, value}}),
                                      "application/json"), 400);
        }
    }
    expect_error(client->Post("/api/simulate",
        simulation_body({{"clients", "500"}, {"ops", "10001"}}), "application/json"), 400);
    expect_error(client->Post("/api/simulate",
        simulation_body({{"accounts", "1"}, {"scenario", R"("OPPOSING")"}}),
        "application/json"), 400);
}

TEST_F(DashboardHttpTest, ValidatesTransactionsBeforeReportingIdleConflict) {
    for (const char* body : {
             "", "{", "[]", "{}", R"({"type":false,"from":1,"amount":"1.00"})",
             R"({"type":"BALANCE","from":1,"amount":"1.00"})",
             R"({"type":"UNKNOWN","from":1,"amount":"1.00"})",
             R"({"type":"DEPOSIT","from":1,"amount":"0.00"})",
             R"({"type":"DEPOSIT","from":1,"amount":0})",
             R"({"type":"WITHDRAW","from":1,"amount":"-0.01"})",
             R"({"type":"DEPOSIT","from":1,"amount":"1.234"})",
             R"({"type":"DEPOSIT","from":1,"amount":"not-money"})",
             R"({"type":"DEPOSIT","from":1,"amount":"92233720368547758.08"})",
             R"({"type":"DEPOSIT","from":1,"amount":true})",
             R"({"type":"DEPOSIT","from":1,"amount":null})",
             R"({"type":"DEPOSIT","from":1})",
             R"({"type":"DEPOSIT","from":0,"amount":"1.00"})",
             R"({"type":"DEPOSIT","from":-1,"amount":"1.00"})",
             R"({"type":"DEPOSIT","from":2147483648,"amount":"1.00"})",
             R"({"type":"DEPOSIT","from":"1","amount":"1.00"})",
             R"({"type":"DEPOSIT","from":1.5,"amount":"1.00"})",
             R"({"type":"TRANSFER","from":1,"amount":"1.00"})",
             R"({"type":"TRANSFER","from":1,"to":0,"amount":"1.00"})",
             R"({"type":"TRANSFER","from":1,"to":-1,"amount":"1.00"})",
             R"({"type":"TRANSFER","from":1,"to":"2","amount":"1.00"})"}) {
        SCOPED_TRACE(body);
        expect_error(client->Post("/api/tx", body, "application/json"), 400);
    }
    expect_error(client->Post("/api/tx",
        R"({"type":"DEPOSIT","from":1,"amount":"1.25"})", "application/json"), 409);
}

TEST_F(DashboardHttpTest, RejectsMalformedMaintenanceBodiesEvenWhenIdle) {
    for (const char* path : {"/api/simulate/stop", "/api/check", "/api/recover"}) {
        for (const char* body : {"{", "[]", "null", R"({"unused":})", "{} trailing"}) {
            SCOPED_TRACE(std::string(path) + " body=" + body);
            expect_error(client->Post(path, body, "application/json"), 400);
        }
    }
    expect_error(client->Post("/api/simulate/stop", "{}", "application/json"), 409);
    for (const char* body : {"{", "[]", "{}", R"({"confirm":false})",
             R"({"confirm":"true"})", R"({"confirm":1})", R"({"confirm":null})"}) {
        SCOPED_TRACE(body);
        expect_error(client->Post("/api/reset", body, "application/json"), 400);
    }
}

TEST_F(DashboardHttpTest, RejectsMalformedMissingUnknownAndWrongTypeDemoNames) {
    for (const char* body : {"", "{", "[]", "{}", R"({"name":"UNKNOWN"})",
             R"({"name":""})", R"({"name":1})", R"({"name":true})", R"({"name":null})",
             R"({"name":"SAFE"} trailing)"}) {
        SCOPED_TRACE(body);
        expect_error(client->Post("/api/demo", body, "application/json"), 400);
    }
}

TEST_F(DashboardHttpTest, RejectsCrossOriginCommandsAndReboundHosts) {
    const auto origin = client->Post("/api/reset", {{"Origin", "https://untrusted.example"}},
                                    R"({"confirm":true})", "application/json");
    expect_error(origin, 403);
    const auto host = client->Get("/api/stats", {{"Host", "untrusted.example"}});
    expect_error(host, 403);
    expect_error(client->Post("/api/reset", R"({"confirm":true})", "text/plain"), 400);
}

std::optional<DbConfig> dashboard_test_config() {
    try {
        DbConfig config = DbConfig::from_environment();
        // Never construct a connection/server with the configured production DB.
        config.database = "bank_test";
        return config;
    } catch (const std::invalid_argument& error) {
        if (std::string_view(error.what()) ==
            "missing required environment variable: BANK_DB_USER") {
            return std::nullopt;
        }
        throw;
    }
}

std::map<int, Money> seeded_baseline() {
    std::map<int, Money> baseline;
    for (int account_id = 1; account_id <= 50; ++account_id) {
        baseline.emplace(account_id, Money::from_minor_units(kOpeningBalanceMinor));
    }
    return baseline;
}

// A transaction-held row lock gates worker completion without timing sleeps.
// Plain dashboard SELECTs remain available through InnoDB consistent reads.
class SeededAccountsGate final {
public:
    explicit SeededAccountsGate(const DbConfig& config) {
        if (config.database != "bank_test") {
            throw std::logic_error("dashboard test row locks require bank_test");
        }
        connection_ = std::make_unique<DbConnection>(config);
        connection_->start_transaction();
        auto statement = connection_->prepare(
            "SELECT account_id FROM accounts ORDER BY account_id FOR UPDATE");
        statement.execute();
        int account_id = 0;
        MYSQL_BIND result{};
        result.buffer_type = MYSQL_TYPE_LONG;
        result.buffer = &account_id;
        result.buffer_length = sizeof(account_id);
        std::array<MYSQL_BIND, 1> results{result};
        statement.bind_results(results);
        statement.store_result();
        int count = 0;
        while (statement.fetch() != MYSQL_NO_DATA) {
            ++count;
        }
        if (count != 50) {
            throw std::runtime_error("bank_test must contain all 50 freshly seeded accounts");
        }
    }

    void release() {
        connection_->rollback();
        connection_.reset();
    }

private:
    // Closing a still-held connection rolls its transaction back on assertion failure.
    std::unique_ptr<DbConnection> connection_;
};

// Integration requirement: CTest must run DashboardDatabaseHttpTest.* with
// RUN_SERIAL (or the entire DB suite with -j1). They reseed shared bank_test.
class DashboardDatabaseHttpTest : public ::testing::Test {
protected:
    void SetUp() override {
        config = dashboard_test_config();
        if (!config) {
            GTEST_SKIP() << "BANK_DB_USER is not configured (environment or .env)";
        }
        ASSERT_EQ(config->database, "bank_test");
        // Configured access-denied, missing-schema and connectivity errors fail.
        // In WSL, a refused Windows-host DB connection is an environment failure,
        // not evidence that these integration assertions passed.
        DbConnection probe(*config);
        server = std::make_unique<DashboardServer>(*config, 0, "web", "results/baseline.txt");
        server->start();
        client = std::make_unique<httplib::Client>("127.0.0.1", server->port());
        configure_client(*client);
        const std::string reset = require_json(
            client->Post("/api/reset", R"({"confirm":true})", "application/json"));
        ASSERT_EQ(json_field(reset, "reset"), "true");
        ASSERT_EQ(integer_field(require_json(client->Get("/api/stats")), "total_balance_minor"),
                  kOpeningTotalMinor);
    }

    void TearDown() override {
        if (server) {
            server->stop();
        }
    }

    void expect_core_invariants() {
        ASSERT_EQ(config->database, "bank_test");
        DbConnection connection(*config);
        const auto report = ConsistencyChecker(connection).check(seeded_baseline());
        EXPECT_TRUE(report.baseline_available);
        EXPECT_TRUE(report.passed);
        EXPECT_TRUE(report.i1_total_conserved);
        EXPECT_TRUE(report.i2_no_negative_balance);
        EXPECT_TRUE(report.i3_final_status);
        EXPECT_TRUE(report.i4_no_pending);
        EXPECT_TRUE(report.i5_ledger_matches);
        EXPECT_EQ(report.pending, 0U);
        EXPECT_EQ(report.ledger_mismatches, 0U);
        for (const auto& violation : report.violations) {
            ADD_FAILURE() << violation;
        }
    }

    std::optional<DbConfig> config;
    std::unique_ptr<DashboardServer> server;
    std::unique_ptr<httplib::Client> client;
};

TEST_F(DashboardDatabaseHttpTest, PaginatesSeededAccountsAndRunsIdleMaintenance) {
    const std::string all = require_json(client->Get("/api/accounts"));
    EXPECT_EQ(integer_field(all, "page"), 1);
    EXPECT_EQ(integer_field(all, "limit"), 50);
    const auto accounts = json_rows(all, "accounts");
    ASSERT_EQ(accounts.size(), 50U);
    for (std::size_t index = 0; index < accounts.size(); ++index) {
        EXPECT_EQ(integer_field(accounts[index], "id"), static_cast<std::int64_t>(index + 1));
        EXPECT_EQ(integer_field(accounts[index], "balance_minor"), kOpeningBalanceMinor);
        EXPECT_EQ(json_field(accounts[index], "balance"), R"("100000.00")");
        EXPECT_EQ(json_field(accounts[index], "status"), R"("ACTIVE")");
        EXPECT_GT(json_field(accounts[index], "owner").size(), 2U);
    }
    for (int page = 1; page <= 9; ++page) {
        const std::string body = require_json(client->Get(
            "/api/accounts?limit=7&page=" + std::to_string(page)));
        EXPECT_EQ(integer_field(body, "page"), page);
        EXPECT_EQ(integer_field(body, "limit"), 7);
        const auto rows = json_rows(body, "accounts");
        const std::size_t expected_count = page <= 7 ? 7U : (page == 8 ? 1U : 0U);
        ASSERT_EQ(rows.size(), expected_count);
        for (std::size_t index = 0; index < rows.size(); ++index) {
            EXPECT_EQ(integer_field(rows[index], "id"), (page - 1) * 7 +
                      static_cast<std::int64_t>(index) + 1);
        }
    }
    EXPECT_EQ(json_rows(require_json(client->Get("/api/accounts?limit=1")), "accounts").size(), 1U);
    EXPECT_EQ(json_rows(require_json(client->Get("/api/accounts?limit=100")), "accounts").size(), 50U);
    EXPECT_TRUE(json_rows(require_json(client->Get("/api/transactions?limit=100")),
                          "transactions").empty());

    expect_invariants(require_json(client->Post("/api/check", "{}", "application/json")));
    const std::string stats = require_json(client->Get("/api/stats"));
    EXPECT_TRUE(std::regex_search(stats, std::regex(R"("latest_check"\s*:\s*\{)")));
    EXPECT_EQ(json_field(stats, "passed"), "true");
    {
        TxnLog log(*config);
        static_cast<void>(log.write_pending("dashboard-http-recovery", 1, "dashboard-http-test"));
    }
    const std::string recovered = require_json(client->Post("/api/recover", "{}", "application/json"));
    EXPECT_EQ(json_field(recovered, "succeeded"), "true");
    EXPECT_EQ(integer_field(recovered, "pending_seen"), 1);
    EXPECT_EQ(integer_field(recovered, "committed"), 0);
    EXPECT_EQ(integer_field(recovered, "rolled_back"), 1);
    EXPECT_EQ(integer_field(recovered, "remaining_pending"), 0);
    const std::string repeated = require_json(client->Post("/api/recover", "{}", "application/json"));
    EXPECT_EQ(json_field(repeated, "succeeded"), "true");
    EXPECT_EQ(integer_field(repeated, "pending_seen"), 0);
    EXPECT_EQ(integer_field(repeated, "remaining_pending"), 0);
    expect_core_invariants();

    EXPECT_EQ(json_field(require_json(client->Post(
        "/api/reset", R"({"confirm":true})", "application/json")), "reset"), "true");
    EXPECT_EQ(json_field(require_json(client->Get("/api/stats")), "latest_check"), "null");
    expect_invariants(require_json(client->Post("/api/check", "{}", "application/json")));
}

TEST_F(DashboardDatabaseHttpTest, LimitsAndFiltersCommittedAndRolledBackTransactions) {
    // Seed both financial statuses through the existing synchronous core API.
    // TransactionManager queue failures are audit-only in the current core API.
    DbConnection connection(*config);
    AccountManager accounts(connection);
    ASSERT_EQ(accounts.deposit("dashboard-history-deposit", 1, Money::from_decimal("1.25")).status,
              TxStatus::Committed);
    ASSERT_EQ(accounts.withdraw("dashboard-history-declined", 1,
                               Money::from_decimal("9999999999.00")).status,
              TxStatus::RolledBack);
    ASSERT_EQ(accounts.transfer("dashboard-history-transfer", 1, 2,
                               Money::from_decimal("1.00")).status,
              TxStatus::Committed);
    const auto ledger = json_rows(require_json(client->Get("/api/transactions")), "transactions");
    ASSERT_EQ(ledger.size(), 3U);
    EXPECT_EQ(json_field(ledger[0], "request_id"), R"("dashboard-history-transfer")");
    EXPECT_EQ(json_field(ledger[1], "request_id"), R"("dashboard-history-declined")");
    EXPECT_EQ(json_field(ledger[2], "request_id"), R"("dashboard-history-deposit")");
    EXPECT_GT(integer_field(ledger[0], "id"), integer_field(ledger[1], "id"));
    EXPECT_GT(integer_field(ledger[1], "id"), integer_field(ledger[2], "id"));
    for (const auto& [filter, count] : std::array<std::pair<const char*, std::size_t>, 4>{{
             {"COMMITTED", 2}, {"committed", 2}, {"ROLLED_BACK", 1}, {"rolled_back", 1}}}) {
        const auto rows = json_rows(require_json(client->Get(
            std::string("/api/transactions?limit=100&status=") + filter)), "transactions");
        ASSERT_EQ(rows.size(), count);
        const std::string status = count == 2 ? R"("COMMITTED")" : R"("ROLLED_BACK")";
        for (const auto& row : rows) {
            EXPECT_EQ(json_field(row, "status"), status);
            EXPECT_EQ(json_field(row, "log_status"), status);
        }
    }
    for (int limit = 1; limit <= 2; ++limit) {
        const auto rows = json_rows(require_json(client->Get(
            "/api/transactions?limit=" + std::to_string(limit))), "transactions");
        ASSERT_EQ(rows.size(), static_cast<std::size_t>(limit));
        EXPECT_EQ(integer_field(rows.front(), "id"), integer_field(ledger.front(), "id"));
    }
    const auto committed = json_rows(require_json(client->Get(
        "/api/transactions?limit=1&status=COMMITTED")), "transactions");
    ASSERT_EQ(committed.size(), 1U);
    EXPECT_EQ(json_field(committed.front(), "request_id"), R"("dashboard-history-transfer")");
    expect_core_invariants();
}

TEST_F(DashboardDatabaseHttpTest, CrashDemoUsesInFlightAbortHookAndPreservesBothAccounts) {
    const std::string before = require_json(client->Get("/api/accounts?limit=2"));
    const auto result = require_json(client->Post("/api/demo", R"({"name":"CRASH_RECOVER"})", "application/json"));
    EXPECT_EQ(json_field(result, "succeeded"), "true") << result;
    EXPECT_EQ(json_field(result, "fault_fired"), "true") << result;
    for (const char* key : {"I1", "I2", "I3", "I4", "I5"}) {
        EXPECT_EQ(json_field(result, key), "true") << result;
    }
    EXPECT_EQ(integer_field(result, "remaining_pending"), 0);
    const std::string after = require_json(client->Get("/api/accounts?limit=2"));
    EXPECT_EQ(json_rows(before, "accounts"), json_rows(after, "accounts"));
    expect_core_invariants();
}

TEST_F(DashboardDatabaseHttpTest, SafeAndRaceDemosReportMeasuredBalanceOutcomes) {
    for (const char* name : {"SAFE", "RACE"}) {
        SCOPED_TRACE(name);
        require_json(client->Post("/api/reset", R"({"confirm":true})", "application/json"));
        require_json(client->Post("/api/demo", "{\"name\":\"" + std::string(name) + "\"}", "application/json"), 202);
        std::string stats;
        ASSERT_TRUE(poll_stats(*client, [](const std::string& value) {
            return json_field(value, "running") == "false";
        }, stats)) << stats;
        EXPECT_EQ(integer_field(stats, "committed"), 100);
        EXPECT_EQ(integer_field(stats, "rolled_back"), 0);
        const auto expected = integer_field(stats, "expected_demo_balance_minor");
        const auto actual = integer_field(stats, "actual_demo_balance_minor");
        EXPECT_EQ(expected, kOpeningBalanceMinor + 100);
        if (std::string_view(name) == "SAFE") {
            EXPECT_EQ(actual, expected);
            expect_core_invariants();
        } else {
            EXPECT_LT(actual, expected) << "unsafe lost updates not observed in this run";
        }
    }
}

TEST_F(DashboardDatabaseHttpTest, DashboardSessionIsolationChoicesKeepTheSafePathCorrect) {
    for (const char* isolation : {"READ_COMMITTED", "REPEATABLE_READ", "SERIALIZABLE"}) {
        SCOPED_TRACE(isolation);
        require_json(client->Post("/api/simulate", simulation_body({
            {"clients", "4"}, {"ops", "5"}, {"workers", "2"},
            {"isolation", '"' + std::string(isolation) + '"'}}), "application/json"), 202);
        std::string stats;
        ASSERT_TRUE(poll_stats(*client, [](const std::string& value) {
            return json_field(value, "running") == "false";
        }, stats)) << stats;
        expect_idle_stats(stats);
        expect_core_invariants();
    }
}

TEST_F(DashboardDatabaseHttpTest, ManualTransactionsUseSimulationQueueCountersAndLedger) {
    const std::array<std::string, 4> bodies{
        R"({"type":"DEPOSIT","from":1,"amount":"1.25"})",
        R"({"type":"WITHDRAW","from":2,"amount":"2.00"})",
        R"({"type":"TRANSFER","from":2,"to":3,"amount":3.50})",
        R"({"type":"WITHDRAW","from":4,"amount":"9999999999.00"})"};
    std::array<std::string, 4> results;
    std::array<std::string, 4> errors;
    std::vector<std::jthread> submitters;
    // Destruction order releases row locks before joining pending HTTP clients.
    SeededAccountsGate gate(*config);
    const std::string started = require_json(client->Post("/api/simulate",
        simulation_body({{"scenario", R"("DEPOSIT")"}}), "application/json"), 202);
    EXPECT_EQ(json_field(started, "started"), "true");
    EXPECT_EQ(json_field(started, "mode"), R"("safe")");
    std::string stats;
    ASSERT_TRUE(poll_stats(*client, [](const std::string& body) {
        return integer_field(body, "active_workers") == 1 && integer_field(body, "queue_depth") == 3;
    }, stats)) << stats;
    expect_error(client->Post("/api/tx",
        R"({"type":"DEPOSIT","from":1,"amount":"0.00"})", "application/json"), 400);
    expect_error(client->Post("/api/tx",
        R"({"type":"UNKNOWN","from":1,"amount":"1.00"})", "application/json"), 400);

    for (std::size_t index = 0; index < bodies.size(); ++index) {
        submitters.emplace_back([&, index] {
            try {
                httplib::Client submitter("127.0.0.1", server->port());
                configure_client(submitter);
                results[index] = require_json(submitter.Post("/api/tx", bodies[index], "application/json"));
            } catch (const std::exception& error) {
                errors[index] = error.what();
            }
        });
    }
    // Four blocked manual requests must increase the *same* queue from 3 to 7.
    const bool shared_queue = poll_stats(*client, [](const std::string& body) {
        return integer_field(body, "queue_depth") == 7;
    }, stats);
    gate.release();
    submitters.clear();
    ASSERT_TRUE(shared_queue) << stats;
    for (std::size_t index = 0; index < results.size(); ++index) {
        SCOPED_TRACE(bodies[index]);
        ASSERT_TRUE(errors[index].empty()) << errors[index];
        EXPECT_EQ(json_field(results[index], "status"),
                  index == 3 ? R"("ROLLED_BACK")" : R"("COMMITTED")");
        EXPECT_GE(integer_field(results[index], "attempts"), 1);
    }
    EXPECT_EQ(json_field(results[3], "reason"), R"("INSUFFICIENT_FUNDS")");
    ASSERT_TRUE(poll_stats(*client, [](const std::string& body) {
        return json_field(body, "running") == "false";
    }, stats)) << stats;
    expect_idle_stats(stats);
    EXPECT_EQ(integer_field(stats, "committed"), 7);
    EXPECT_EQ(integer_field(stats, "rolled_back"), 1);
    EXPECT_EQ(integer_field(stats, "INSUFFICIENT_FUNDS"), 1);
    EXPECT_EQ(integer_field(stats, "total_balance_minor"), kOpeningTotalMinor + 4 + 125 - 200);

    const auto accounts = json_rows(require_json(client->Get("/api/accounts?limit=4")), "accounts");
    ASSERT_EQ(accounts.size(), 4U);
    const std::array<std::int64_t, 4> expected_balances{
        kOpeningBalanceMinor + 4 + 125, kOpeningBalanceMinor - 200 - 350,
        kOpeningBalanceMinor + 350, kOpeningBalanceMinor};
    for (std::size_t index = 0; index < accounts.size(); ++index) {
        EXPECT_EQ(integer_field(accounts[index], "balance_minor"), expected_balances[index]);
    }

    const auto ledger = json_rows(require_json(client->Get("/api/transactions?limit=100")), "transactions");
    ASSERT_EQ(ledger.size(), 7U);
    std::int64_t previous_id = integer_field(ledger.front(), "id") + 1;
    std::set<std::string> request_ids;
    for (const auto& row : ledger) {
        EXPECT_LT(integer_field(row, "id"), previous_id);
        previous_id = integer_field(row, "id");
        EXPECT_EQ(json_field(row, "log_status"), json_field(row, "status"));
        EXPECT_GT(json_field(row, "created_at").size(), 2U);
        const std::string request_id = json_field(row, "request_id");
        EXPECT_GT(request_id.size(), 2U);
        EXPECT_TRUE(request_ids.insert(request_id).second) << request_id;
    }
    struct ExpectedLedgerRow {
        const char* amount;
        const char* type;
        int from;
        int to;
        std::size_t count;
    };
    const std::array<ExpectedLedgerRow, 4> expected_ledger{{
        {"0.01", "DEPOSIT", 0, 1, 4}, {"1.25", "DEPOSIT", 0, 1, 1},
        {"2.00", "WITHDRAW", 2, 0, 1}, {"3.50", "TRANSFER", 2, 3, 1}}};
    for (const auto& expected : expected_ledger) {
        std::size_t count = 0;
        for (const auto& row : ledger) {
            if (json_field(row, "amount") != '"' + std::string(expected.amount) + '"') {
                continue;
            }
            ++count;
            EXPECT_EQ(json_field(row, "type"), '"' + std::string(expected.type) + '"');
            EXPECT_EQ(integer_field(row, "from"), expected.from);
            EXPECT_EQ(integer_field(row, "to"), expected.to);
            EXPECT_EQ(json_field(row, "status"), R"("COMMITTED")");
        }
        EXPECT_EQ(count, expected.count) << expected.amount;
    }
    expect_core_invariants();
    expect_invariants(require_json(client->Post("/api/check", "{}", "application/json")));
    expect_error(client->Post("/api/tx", bodies[0], "application/json"), 409);

    // Reset must remove real ledger/audit history and restore opening balances.
    require_json(client->Post("/api/reset", R"({"confirm":true})", "application/json"));
    EXPECT_TRUE(json_rows(require_json(client->Get("/api/transactions")), "transactions").empty());
    EXPECT_EQ(integer_field(require_json(client->Get("/api/stats")), "total_balance_minor"),
              kOpeningTotalMinor);
    expect_core_invariants();
}

TEST_F(DashboardDatabaseHttpTest, ConcurrentReadEndpointsRemainAvailableDuringHundredClientSimulationAndStop) {
    constexpr std::size_t kReaders = 20;
    std::atomic<int> failures{0};
    std::array<std::string, kReaders> reader_errors;
    std::latch readers_ready(kReaders);
    std::latch first_reads_done(kReaders);
    std::latch start_reading(1);
    std::latch continue_reading(1);
    std::latch stoppers_ready(2);
    std::latch start_stopping(1);
    std::vector<std::jthread> readers;
    std::array<int, 2> stop_statuses{};
    std::array<std::string, 2> stop_errors;
    std::vector<std::jthread> stoppers;
    SeededAccountsGate gate(*config);
    // Release waiters before jthread destruction even if an HTTP/DB call throws.
    LatchRelease release_start_reading(start_reading);
    LatchRelease release_continue_reading(continue_reading);
    LatchRelease release_start_stopping(start_stopping);
    const std::string body = simulation_body({{"clients", "100"}, {"ops", "20"},
        {"workers", "8"}, {"hotspot_ratio", "0.25"}});
    require_json(client->Post("/api/simulate", body, "application/json"), 202);
    expect_error(client->Post("/api/simulate", body, "application/json"), 409);
    for (const char* path : {"/api/check", "/api/recover"}) {
        expect_error(client->Post(path, "{}", "application/json"), 409);
    }
    expect_error(client->Post("/api/reset", R"({"confirm":true})", "application/json"), 409);
    expect_error(client->Post("/api/reset", R"({"confirm":false})", "application/json"), 400);

    for (std::size_t index = 0; index < kReaders; ++index) {
        readers.emplace_back([&, index] {
            readers_ready.count_down();
            start_reading.wait();
            for (int iteration = 0; iteration < 5; ++iteration) {
                try {
                    httplib::Client reader("127.0.0.1", server->port());
                    configure_client(reader);
                    const std::string stats = require_json(reader.Get("/api/stats"));
                    const std::string page = require_json(reader.Get(
                        "/api/accounts?limit=10&page=" + std::to_string(index % 5 + 1)));
                    const auto accounts = json_rows(page, "accounts");
                    if ((iteration == 0 && json_field(stats, "running") != "true") ||
                        json_field(stats, "mode") != R"("safe")" ||
                        integer_field(stats, "workers") != 8 ||
                        integer_field(stats, "active_workers") > 8 ||
                        integer_field(stats, "pool_leases_in_use") > 8 ||
                        integer_field(stats, "queue_depth") > 512 ||
                        integer_field(stats, "total_balance_minor") < 0 || accounts.size() != 10) {
                        throw std::runtime_error("invalid concurrent stats/page: " + stats + " " + page);
                    }
                    for (std::size_t row = 0; row < accounts.size(); ++row) {
                        const auto expected_id = static_cast<std::int64_t>((index % 5) * 10 + row + 1);
                        if (integer_field(accounts[row], "id") != expected_id ||
                            integer_field(accounts[row], "balance_minor") < 0) {
                            throw std::runtime_error("invalid concurrent account row: " + accounts[row]);
                        }
                    }
                } catch (const std::exception& error) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    reader_errors[index] = error.what();
                }
                if (iteration == 0) {
                    first_reads_done.count_down();
                    continue_reading.wait();
                }
            }
        });
    }
    readers_ready.wait();
    release_start_reading.release();
    first_reads_done.wait();

    // Keep reads in flight while two HTTP handlers race to stop/drain one run.
    for (std::size_t index = 0; index < stop_statuses.size(); ++index) {
        stoppers.emplace_back([&, index] {
            stoppers_ready.count_down();
            start_stopping.wait();
            try {
                httplib::Client stopper("127.0.0.1", server->port());
                configure_client(stopper);
                const auto response = stopper.Post("/api/simulate/stop", "{}", "application/json");
                if (!response) {
                    throw std::runtime_error(httplib::to_string(response.error()));
                }
                stop_statuses[index] = response->status;
                if (response->status == 200) {
                    if (json_field(response->body, "stopped") != "true") {
                        throw std::runtime_error(response->body);
                    }
                } else {
                    const auto error = require_json(response, 409);
                    if (json_field(error, "reason").size() <= 2) {
                        throw std::runtime_error("missing stop conflict reason: " + error);
                    }
                }
            } catch (const std::exception& error) {
                stop_errors[index] = error.what();
            }
        });
    }
    stoppers_ready.wait();
    release_start_stopping.release();
    gate.release();
    release_continue_reading.release();
    stoppers.clear();
    readers.clear();

    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
    for (const auto& error : reader_errors) {
        EXPECT_TRUE(error.empty()) << error;
    }
    for (std::size_t index = 0; index < stop_statuses.size(); ++index) {
        EXPECT_TRUE(stop_errors[index].empty()) << stop_errors[index];
        EXPECT_TRUE(stop_statuses[index] == 200 || stop_statuses[index] == 409);
    }
    EXPECT_TRUE(stop_statuses[0] == 200 || stop_statuses[1] == 200);
    const std::string stopped_stats = require_json(client->Get("/api/stats"));
    expect_idle_stats(stopped_stats);
    EXPECT_EQ(json_field(stopped_stats, "mode"), R"("safe")");
    EXPECT_GE(integer_field(stopped_stats, "total_balance_minor"), 0);
    EXPECT_EQ(integer_field(stopped_stats, "DB_ERROR"), 0);
    EXPECT_EQ(integer_field(stopped_stats, "WORKER_ERROR"), 0);
    EXPECT_GT(integer_field(stopped_stats, "committed") + integer_field(stopped_stats, "rolled_back"), 0);
    expect_error(client->Post("/api/simulate/stop", "{}", "application/json"), 409);
    // Check before recovery so recovery cannot conceal unresolved PENDING rows.
    expect_core_invariants();
    expect_invariants(require_json(client->Post("/api/check", "{}", "application/json")));
    const std::string recovery = require_json(client->Post("/api/recover", "{}", "application/json"));
    EXPECT_EQ(json_field(recovery, "succeeded"), "true");
    EXPECT_EQ(integer_field(recovery, "pending_seen"), 0);
    EXPECT_EQ(integer_field(recovery, "remaining_pending"), 0);
}

}  // namespace
}  // namespace bank
