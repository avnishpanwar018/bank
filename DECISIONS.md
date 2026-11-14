# DECISIONS.md — Architecture Decision Records

Format: **Context → Decision → Consequences**. Add new entries at the bottom; never delete, mark `Superseded`.

---
## ADR-001: Language = C++20
- **Status:** Accepted (change if course mandates otherwise)
- **Context:** OS project should expose real threads, mutexes and semaphores; DSA/CP background fits C++.
- **Decision:** C++20 with `std::thread`, `std::mutex`, `std::counting_semaphore`.
- **Consequences:** Strong OS-concept mapping and TSan support; more boilerplate than Python/Java. Alternative: Java (`ReentrantLock`, `Semaphore`, JDBC) or C with pthreads.

## ADR-002: Two-layer concurrency (mutex + DB row locks)
- **Status:** Accepted
- **Context:** Project must show both OS synchronization and DBMS concurrency control.
- **Decision:** In-process per-account mutexes (OS) **and** `SELECT ... FOR UPDATE` inside DB transactions (DBMS).
- **Consequences:** Demonstrates both subjects and gives defense in depth; slightly lower throughput. Mutex layer can be toggled off (`--unsafe`) for demos.

## ADR-003: Deadlock avoidance by global lock ordering
- **Status:** Accepted
- **Context:** Opposing transfers (A→B, B→A) can deadlock.
- **Decision:** Always acquire in ascending account id (mutexes and `ORDER BY ... FOR UPDATE`). Retry on MySQL 1213/1205 as fallback.
- **Consequences:** Circular wait impossible in mutex layer; DB retries cover residual cases.

## ADR-004: Bounded producer–consumer queue with semaphores
- **Status:** Accepted
- **Decision:** Queue uses a mutex plus `empty`/`full` counting semaphores; fixed worker pool.
- **Consequences:** Back-pressure instead of unbounded memory; classic OS textbook pattern.

## ADR-005: Connection pool guarded by counting semaphore
- **Status:** Accepted
- **Decision:** Pool of K MySQL connections (default 8); `acquire()` waits on semaphore.
- **Consequences:** Prevents "too many connections"; K tunable.

## ADR-006: Isolation level = REPEATABLE READ + explicit FOR UPDATE
- **Status:** Accepted
- **Context:** InnoDB default is REPEATABLE READ; SERIALIZABLE is slower.
- **Decision:** Keep default and lock rows explicitly.
- **Consequences:** Good throughput, prevents lost updates; isolation experiments (READ COMMITTED / SERIALIZABLE) can be run for the report.

## ADR-007: Separate connection for PENDING log
- **Status:** Accepted
- **Decision:** Log writes use their own autocommit connection.
- **Consequences:** PENDING survives rollback/crash, enabling recovery and audit; extra connection use.

## ADR-008: Money as integer minor units
- **Status:** Accepted
- **Decision:** `int64_t` paise in C++, `DECIMAL(15,2)` in SQL.
- **Consequences:** No floating-point errors.

## ADR-009: Idempotency via `request_id`
- **Status:** Accepted
- **Decision:** UUID per request with `UNIQUE` constraint in `transactions`.
- **Consequences:** Safe retries; duplicate submissions rejected.

## ADR-010: CLI + live monitor (no web UI)
- **Status:** Accepted
- **Context:** Scope and time; "clear interface for monitoring" is met by monitor output.
- **Consequences:** Optional web dashboard moved to stretch goals.

## ADR-011: Unsafe mode included for demonstration
- **Status:** Accepted
- **Decision:** `--unsafe` skips mutexes and `FOR UPDATE` (read-modify-write in app).
- **Consequences:** Visible lost updates in the demo; must never be default.

## ADR-013: MySQL C API transaction controls
- **Status:** Accepted
- **Context:** MySQL 8 rejects `START TRANSACTION` and transaction-control
  statements sent through the prepared-statement protocol, although all data
  statements continue to use prepared statements.
- **Decision:** Use `mysql_real_query` for the static `START TRANSACTION` command
  and the MySQL C API `mysql_commit`/`mysql_rollback` calls for transaction
  completion. No user data is interpolated into these control operations.
- **Consequences:** The implementation preserves one-request/one-transaction
  semantics and works with MySQL Connector/C; all value-bearing SQL remains
  prepared and bound.

## ADR-012: Phase 2 validation, logging, and test-database boundaries
- **Status:** Accepted
- **Context:** Phase 2 needs deterministic single-threaded F1–F9 behavior before the
  later TransactionManager and recovery phases are implemented. The existing
  requirements leave the status of validation failures, the shape of final log
  entries, balance-read history, and the phase boundary ambiguous.
- **Decision:** Invalid amounts and self-transfers execute the same request
  transaction and finish as `ROLLED_BACK`. The phase-2 implementation updates the
  request's `PENDING` log row in place. A duplicate `request_id` returns the
  previously recorded result and does not apply the operation again. Balance checks
  are read-only and are not inserted into the financial `transactions` table.
  Positive-amount validation failures also receive a durable `ROLLED_BACK` row in
  `transactions`; non-positive amounts are represented only in `transaction_log`
  because the schema's amount check intentionally rejects them.
  C++ money values are cents/paise; the seed value `100000.00` is represented as
  10,000,000 minor units in C++. Full recovery and FR1 customer/account creation
  remain outside the Phase 0–2 implementation scope.
- **Consequences:** The phase-2 API is small and deterministic, while the later
  TransactionManager can own retry, separate write-ahead logging, and recovery.
  Integration tests require a pre-created `bank_test` database populated with the
  same schema and seed data as the development database.

## ADR-014: Phase 5 transaction ownership and invariant baseline
- **Status:** Accepted
- **Context:** Phase 3–4 placed transaction boundaries and audit writes inside
  `AccountManager`, which prevented retry, recovery, ledger replay, and bounded
  audit-connection testing from being implemented cleanly.
- **Decision:** `TransactionManager` owns request serialization, PENDING/final
  audit resolution, ordered account-lock acquisition, transaction begin/commit/
  rollback, and retries for MySQL 1213/1205. `AccountManager` exposes transaction-
  context operations for SQL/business rules. `TxnLog` uses one persistent
  autocommit connection per manager. `Recovery` reconciles PENDING rows using the
  financial ledger. `ConsistencyChecker` evaluates I1–I5 when supplied a complete
  opening-balance baseline; the baseline is external because the current schema
  does not persist opening balances.
- **Consequences:** D1/D2 connection churn is bounded and D3/I5/X1 can be tested
  deterministically. Full crash/connection replacement coverage still requires
  explicit process and broken-lease test seams; no opening-balance schema change
  is introduced in this phase.

## ADR-015: Phase 7 CLI monitor and startup orchestration
- **Status:** Accepted
- **Context:** Phase 7 requires a useful CLI demonstration and live observability,
  while the database schema does not persist opening balances and the monitor
  must not add database polling or synchronization sleeps.
- **Decision:** `--verbose` starts one joined monitor thread that reads relaxed
  atomic counters and bounded-queue occupancy. Counters include active worker
  operations, service latency, and contended in-process account-mutex
  acquisitions; lock-wait counts are explicitly not MySQL wait metrics. Normal
  simulation performs recovery before starting clients. Maintenance mode keeps
  `--recover` recovery-only, requires `--baseline-file` for `--check`, and allows
  `--recover --check` in that order.
- **Consequences:** Short runs still emit an initial monitor snapshot, shutdown
  remains condition-variable driven and fully joined, and the CLI has one clear
  recovery/check path. The external baseline remains required for I1/I5, and
  process-level X2/X3 seams are still separate work.

## ADR-016: Optional localhost web dashboard stretch feature
- **Status:** Accepted
- **Context:** The core project was scoped as CLI-only, but the Phase 8 stretch
  goals allow a visual live demonstration. A dashboard must not weaken the
  transaction engine, expose the service beyond the demo machine, or become a
  second path for changing balances.
- **Decision:** Amend the PRD non-goal from “no web UI” to “no required web UI
  for the core engine; an optional read-mostly web dashboard is allowed as a
  stretch/demo feature.” Add a localhost-only HTTP server and static vanilla
  JavaScript frontend. The dashboard may start/stop simulations and submit
  manual requests only through the existing request queue and
  `TransactionManager`; read endpoints use snapshots or prepared read queries.
  The server binds only to `127.0.0.1`, has no authentication because it is a
  local demo tool, and never holds account or queue locks while serving HTTP.
  The core engine remains the default and its behavior is unchanged.
- **Consequences:** The project gains a visual demonstration surface without
  changing banking invariants or adding a required web deployment. The server
  and frontend add optional build/runtime surface, must be tested under normal
  and ThreadSanitizer builds, and must be documented as localhost-only with no
  production security guarantees.

---
## Template
```
## ADR-0XX: <title>
- Status: Proposed | Accepted | Superseded by ADR-0YY
- Context:
- Decision:
- Consequences:
```
