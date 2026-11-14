# MEMORY.md — Living Project State

> Update at the end of every work session. Agents: read this before coding.

## Project Snapshot
- **Name:** Multithreaded Bank Transaction Management System with Concurrency Control
- **Subjects:** Operating Systems + DBMS
- **Stack:** C++20, std::thread/mutex/counting_semaphore, MySQL 8 (InnoDB), CMake, GoogleTest
- **Current phase:** Optional dashboard stretch (Phase 8 documentation complete; dashboard implementation and verification in progress)
- **Last updated:** 2026-10-20

## Latest continuation — containerization and full regression verified (2026-11-15)

- Full regression test suite verified on native Windows with CMake & GCC: 55 discovered, 54 passed, 1 expected dotenv skip in ~797s.
- Containerized Linux build fully repaired and verified:
  - Upgraded Dockerfile to multi-stage Ubuntu 22.04 build compiling native Linux ELF binaries (`bank`, `bank_tests`).
  - Added `C.UTF-8` locale and `python3` dependency in builder stage to fix libarchive UTF-8 extraction and cpp-httplib configuration.
  - Adjusted `CMakeLists.txt` for backward-compatible `CMP0135` policy and removed CMake 3.24-specific `DOWNLOAD_EXTRACT_TIMESTAMP` keyword from `FetchContent_Declare`.
  - Added `.dockerignore` to keep host builds and binaries out of Docker context.
  - Updated `docker-compose.yml` to automatically mount all four schema and seed SQL scripts (`01_schema.sql`, `02_seed.sql`, `03_test_schema.sql`, `04_test_seed.sql`) so both `bank` and `bank_test` initialize correctly.
  - Resolved `MYSQL_USER="root"` collision in MySQL 8 container entrypoint.
  - Verified `docker-compose up` completes a 500-transaction simulation with live monitoring and exits 0.
  - Verified `docker-compose run --rm bank /usr/local/bin/bank --check --baseline-file results/baseline.txt` passes all 5 invariants (I1–I5).
  all five invariants, and abort/rollback/recovery. It refreshed all three PNGs in
  `docs/img/`; the check image now shows green I1-I5. This helper explicitly reseeds
  `bank_test`, requires Chromium and Python `websockets`, and must not run alongside
  integration tests. It stopped its server through the CLI Enter-to-stop path.
- After the user started `MySQL80`, the history-filter/audit-status test passed
  **three consecutive targeted reruns**, then passed in the focused and full runs.
  No code change was needed. The earlier outage's underlying cause is unknown;
  the audit failure did not reproduce with the service healthy.
- Latest focused dashboard rerun: **20/20 passed** (18.687 seconds).
- Latest full serial CTest: **55 discovered, 54 passed, 0 failed, 1 expected dotenv
  skip** in **597.07 seconds**. This supersedes the earlier 31-pass/23-failure run
  during the MySQL outage. `MySQL80` was still Running after the clean full run.
- WSL launches again. Current Debug TSan configure and single-job build passed.
  **20 database-independent TSan tests passed**, including dashboard HTTP,
  counters, queue, lock manager, monitor, and money, with no sanitizer report.
- The WSL TSan DB smoke reaches Windows MySQL through the detected gateway but
  the configured account is rejected from the WSL host (access denied). Full
  DB-backed TSan was not run; it needs authorized test-account/host configuration.
  No MySQL accounts, grants, credentials, or service settings were changed.
- The old 1473.9/1223.0 ms timing smoke is not a controlled performance comparison
  (different seeds/idempotent request reuse may confound it); the <=5% acceptance
  criterion remains unverified.
- Next: configure an authorized WSL MySQL test account privately, then run the
  DB smoke and complete `/home/hp/bank-build-tsan` CTest suite using the Windows
  gateway as `BANK_DB_HOST`. Also capture a controlled idle-dashboard performance
  comparison; do not claim full dashboard exit before these gates pass.

## Status Board
| Phase | Status | Notes |
|-------|--------|-------|
| 0 Requirements | Complete locally | CMake project and source tree build with GCC; MySQL connection exit deferred |
| 1 Design | Complete | Existing design docs reviewed; ADR-012 added for Phase 2 boundaries |
| 2 DB implementation | Complete | Schema, seed, RAII DB wrapper, Money, AccountManager, and F1–F9 pass against bank_test |
| 3 Multithreading | Complete functionally | BoundedQueue, ThreadPool, ConnectionPool, Simulator, unsafe mode, S3 and R2 complete; normal CTest passes 24/24 with Phase 4 tests included |
| 4 Synchronization | Complete functionally | Ordered RAII LockManager, safe-path integration, R1/R3/D1/D2 pass |
| 5 Transaction mgmt | In progress | TransactionManager, retry hook, TxnLog, Recovery, ConsistencyChecker, X1/X4/I5 added; X2/X3 seams remain |
| 6 Testing | In progress | Traceability, S1–S4, sanitizer infrastructure, benchmark artifacts, report added; S5 50/50 and DB sanitizer runs pending |
| 7 Integration | In progress | Monitor, startup recovery, CLI maintenance modes, README, and one-command scripts added; final end-to-end run remains environment-dependent |
| 8 Docs & presentation | Complete with limitations | Final report, presentation outline, diagrams, performance references, and guided demo scripts added; live DB rehearsal depends on valid MySQL credentials |
| Stretch dashboard | In progress | Localhost-only cpp-httplib server, JSON/SSE API, offline vanilla dashboard, screenshots, and HTTP tests added; full TSan/idle-regression verification remains |

## Core Invariants (never break)
1. Sum of all balances is conserved by transfers
2. No account balance is ever negative
3. Every request ends as exactly one of COMMITTED / ROLLED_BACK
4. Locks are always acquired in ascending account-id order

## Key Facts / Glossary
- **Request:** one deposit/withdraw/transfer/balance op with a unique `request_id`
- **PENDING log row:** written before the tx starts; resolved after
- **Unsafe mode:** deliberately removes synchronization to demo races
- **Money:** integer minor units in C++

## Environment
- DB: `bank` @ localhost:3306, user via `BANK_DB_USER`
- Build dirs: `build/` (normal), `build-tsan/` (ThreadSanitizer)

## Gotchas / Lessons Learned
- This Windows environment has MySQL Server 8.0 headers and client DLLs; CMake 4.4.4 is installed at `C:\Program Files\CMake\bin`, but that directory is not visible on the current tool PATH, so verification invokes the absolute CMake/CTest paths.
- GCC/MinGW configuration is now supported when `MYSQL_LIBRARY` resolves to `libmysql.dll`; the CMake build and executable test discovery complete successfully.
- `DbConfig::from_environment()` now loads a local `.env` file by searching from the current directory upward; already-exported `BANK_DB_*` variables take precedence. Use `.env.example` as the redacted template, and never commit `.env`.
- Integration tests initially skipped because their pre-check inspected `getenv()` before the application loaded `.env`; the tests now call `DbConfig::from_environment()` first, so `.env`-only configuration works.
- MySQL 8 does not support `START TRANSACTION` through the prepared-statement protocol; ADR-013 uses the C API for static transaction controls while keeping all value-bearing SQL prepared.
- The available compiler is MinGW g++; the installed MySQL `.lib` is an MSVC import library, so direct MinGW linking fails. Use a matching MSVC/CMake toolchain or install a MinGW-compatible MySQL/MariaDB client import library.
- Integration tests use `bank_test`; initialize it with `sql/test_schema.sql` and `sql/test_seed.sql` before execution. Credentials remain in `BANK_DB_*` only.
- A post-commit audit failure can leave a `PENDING` log row; Phase 5 recovery/reconciliation must resolve this ambiguous outcome before the full I4 recovery criterion is claimed.
- Phase 2 deliberately keeps full recovery, retry policy, connection pooling, and FR1 account creation for later phases.
- Phase 3 keeps queue, promise, connection-lease, and thread lifecycle synchronization correct; only the unsafe AccountManager path bypasses row locking and writes stale absolute balances.
- Windows MinGW GCC 15.2.0 in this environment has no `libtsan`, so `-DSANITIZE=thread` compiles objects but fails at link with `cannot find -ltsan`; run TSan under Linux/WSL or install a toolchain that supplies it.
- The Windows MinGW TSan build reaches linking and fails because MinGW lacks `libtsan`; WSL2 is now the working TSan toolchain after installing its Linux build tools.
- WSL2 can build and run the TSan binary after the user installed the Linux toolchain. The source-tree CTest working directory fix makes `.env` discoverable, but WSL initially used `127.0.0.1` (its own network namespace); using the Windows gateway reaches MySQL but the configured account is still rejected for the WSL host.
- The first WSL gateway retry used the literal placeholder password `your_password`; all 17 DB-dependent TSan tests consequently failed with MySQL `Access denied`. No TSan race result is claimed for those tests until the actual password and matching `bank_tsan` grant are used.
- `LockManager` owns stable per-account mutexes in a registry and returns move-only `ScopedLocks`; requested IDs are sorted and deduplicated before any account mutex is acquired. Invalid account IDs are still locked as registry keys so the DB path can return `ACCOUNT_NOT_FOUND`.
- Account logging remains outside the LockManager critical section: PENDING is written before account locks, and final audit resolution occurs after the transaction lock scope is released.
- Phase 5 moved transaction ownership out of AccountManager into TransactionManager; the persistent TxnLog reduced audit-connection churn and prevented the earlier D1 Windows 10048 failure pattern in targeted runs.
- ConsistencyChecker requires a complete opening-balance baseline because the current schema does not persist opening balances; `results/baseline.txt` is a report fixture, not a secret.
- `scripts/repeat_stress.sh` is prepared for S5 but was not allowed to claim 50/50 in this session because each full S1 run takes roughly two minutes locally.
- `scripts/bench.sh` labels unsupported isolation/lock-ordering/hotspot dimensions as `NOT_IMPLEMENTED` rather than inventing measurements; the current worker CSV uses measured CLI runs.
- The historical Phase 5 full normal run passed its then-current 34-test suite. The latest dashboard tree discovers 55 tests: 54 passed, zero failed, one expected dotenv skip.
- WSL TSan full run passed all 34 tests with one expected dotenv skip and no race/lock-order report. WSL ASan passed 33/34: S1 observed seven unresolved PENDING rows while I1/I2/I3/I5 passed; S2, S3, S4 and all other tests passed. The audit resolver now has a fallback connection and final-status reconciliation; rerun ASan S1 to confirm the fix.
- S5 was started under WSL and `results/s5_summary.csv` now records seeds 1–16 with I1–I5 true; `scripts/repeat_stress.sh` resumes from the last recorded seed instead of overwriting the summary. Seeds 17–50 remain pending.
- Phase 7 adds `Monitor`, relaxed atomic active/latency/lock-wait counters, `--verbose`, automatic startup recovery, recovery-only `--recover`, baseline-required `--check`, and combined `--recover --check` maintenance mode. The monitor uses a condition variable and a joined thread; it never polls MySQL.
- `scripts/build.sh`/`demo.sh`/`check.sh` and PowerShell equivalents are available. Native Windows defaults to `build-gcc`; Linux/WSL defaults to `${HOME}/bank-build` and limits default CMake parallelism to two jobs to avoid mounted-filesystem and memory failures.
- Phase 5 test reset now restores all 50 seeded accounts before applying scenario-specific balances, preventing I5 from depending on earlier stress-test mutations.
- Post-Phase-7 Windows build passed; the monitor test passed; a 30-test non-stress/non-D1 regression subset passed with one expected dotenv skip; targeted S1 passed after the test-isolation fix; and a real one-client `--verbose` CLI smoke run printed startup recovery, monitor snapshots, and final metrics.
- Post-Phase-7 WSL TSan configure/build passed and the monitor test passed. The full TSan test invocation was blocked by WSL MySQL connectivity (`127.0.0.1:3306` refused), so no new full post-change TSan result is claimed.
- ADR-016 amends the PRD non-goal only for an optional read-mostly localhost dashboard. The core engine remains the transaction/queue path; the dashboard server binds to `127.0.0.1`, has no auth, and manual writes enter the existing queue/TransactionManager.
- cpp-httplib 0.18.3 is fetched by CMake. `web/index.html`, `web/app.js`, and `web/style.css` are buildless vanilla frontend assets; Chart.js is vendored locally under `web/vendor/`.
- Dashboard HTTP smoke tests pass without a database for static frontend, stats, and validation routes. The DB-backed concurrent-read test passed in a reachable configured run; if credentials are unavailable it skips by design.
- Refreshed real-browser captures are in `docs/img/dashboard-overview.png`, `dashboard-race.png`, and `dashboard-check.png`; the check image now shows green I1-I5 after resetting the unsafe state and running safe deposits/opposing transfers.
- The dashboard race/safe demo stats expose expected account-1 balance, actual balance, and lost-update delta. The historical 1473.9/1223.0 ms timing smoke was confounded and is not evidence for the <=5% overhead gate.
- The complete Windows normal suite after the dashboard was added passed 38/38, with only the expected dotenv test skipped. The complete WSL TSan attempt still fails only at DB-dependent tests because WSL cannot reach MySQL at `127.0.0.1:3306`; dashboard static/validation and monitor TSan tests pass.

## Phase 3 R2 Results
Run: `UnsafeRaceTest.R2ReportsLostUpdatesAcrossTwentyRuns`, 100 unsafe deposits of 1 cent, initial balance 10000 cents, 16 workers/connections.

| Run | Expected | Actual | Lost |
|-----|----------|--------|------|
| 1 | 10100 | 10008 | 92 |
| 2 | 10100 | 10008 | 92 |
| 3 | 10100 | 10008 | 92 |
| 4 | 10100 | 10008 | 92 |
| 5 | 10100 | 10008 | 92 |
| 6 | 10100 | 10008 | 92 |
| 7 | 10100 | 10008 | 92 |
| 8 | 10100 | 10008 | 92 |
| 9 | 10100 | 10008 | 92 |
| 10 | 10100 | 10008 | 92 |
| 11 | 10100 | 10008 | 92 |
| 12 | 10100 | 10008 | 92 |
| 13 | 10100 | 10009 | 91 |
| 14 | 10100 | 10008 | 92 |
| 15 | 10100 | 10008 | 92 |
| 16 | 10100 | 10008 | 92 |
| 17 | 10100 | 10008 | 92 |
| 18 | 10100 | 10008 | 92 |
| 19 | 10100 | 10008 | 92 |
| 20 | 10100 | 10008 | 92 |

Lost-update runs: **20/20**.

## Phase 4 Synchronization Results
- **R1:** 100 safe concurrent deposits of 1 cent to one account; passed with final balance equal to the expected start plus 100 cents.
- **R3:** 50 safe concurrent withdrawals of 10.00 from a 100.00 balance; passed with exactly 10 commits, final balance 0.00, and no negative balance.
- **D1:** 10,000 opposing transfers in each direction; passed without deadlock/hang. The test accepts committed or rolled-back requests because Phase 5 retry logic is not implemented yet, and verifies non-negative balances plus total conservation. One observed run completed with 8,115 commits and 11,885 rollbacks, balances 99.97/100.03, total 200.00.
- **D2:** 1,000 transfers on each edge of the A→B→C→A ring; passed without deadlock/hang, with non-negative balances and total conservation.
- **Lock-order rule:** every multi-account request goes through `LockManager::lock`, which sorts ascending account IDs before constructing RAII `unique_lock` objects. No manual mutex lock/unlock or sleep-based synchronization was added.
- **TSan:** WSL2 TSan passed the six non-DB tests (three BoundedQueue/S3 tests, LockManager unit test, and two Money tests) with no race report. DB-dependent TSan tests require a MySQL user grant for the WSL gateway host before they can run.
- **TSan/ASan update:** WSL2 TSan and ASan configure/builds succeeded after Phase 5 additions. Non-DB queue/lock/money gates passed with no sanitizer report. DB-dependent runs remain blocked by the WSL MySQL grant/password setup.

## Open Questions
- Final language confirmed with course instructor? (default C++20)
- The dashboard remains optional; the PRD now permits a read-mostly localhost-only web demo under ADR-016.

## Session Log
| Date | Done | Next |
|------|------|------|
| 2026-10-16 | Added Phase 0 scaffold, schema/seed, Money/types, MySQL RAII wrapper, single-threaded AccountManager, smoke/F1–F9 tests, ADR-012; configured GCC/CMake and verified unit tests | Configure `.env` when needed; begin Phase 3 only after review |
| 2026-10-16 | Fixed `.env` test pre-check and MySQL transaction-control incompatibility; all 13 CTest tests pass, including DB smoke and F1–F9 | Start Phase 3 when authorized |
| 2026-10-16 | Implemented BoundedQueue, ThreadPool, ConnectionPool, Simulator CLI, unsafe read-modify-write path, S3, pool/thread-pool tests, and R2; normal build and 19/19 CTest pass; safe/unsafe CLI smoke runs complete | Run TSan under Linux/WSL or install MinGW libtsan; then begin Phase 4 |
| 2026-10-16 | Read Phase 4 documents in requested order; ran the existing 19/19 normal suite; confirmed WSL2 exists but lacks CMake/GCC | Implement ordered LockManager and safe-path tests |
| 2026-10-16 | Added ordered RAII LockManager, integrated it into safe ThreadPool/AccountManager operations, added R1/R3/D1/D2 and LockManager tests; normal CTest passes 24/24 | Install a Linux/WSL TSan toolchain and run `-DSANITIZE=thread`; then Phase 4 can be closed |
| 2026-10-16 | WSL2 TSan configure/build succeeded; after setting the CTest working directory to the source tree, non-DB TSan tests passed with no race reports. DB tests reach MySQL only through the WSL gateway and currently receive `Host '172.23.127.33' is not allowed to connect`; Windows normal D1 later hit MySQL client error 10048 after its high-volume audit connections | Grant the test DB user for the WSL gateway host, run DB-dependent TSan tests, then rerun D1 after MySQL client ports recover |
| 2026-10-16 | WSL gateway connectivity works, but the DB-dependent TSan run used the placeholder `your_password` and received `Access denied for user 'bank_tsan'@'172.23.127.33'`; non-DB TSan remains clean | Replace the placeholder with the actual matching password, then rerun the DB-dependent TSan filter |
| 2026-10-16 | Completed minimal Phase 5 transaction ownership/retry/log/recovery/checker layer; D3, X1, X4, I5, recovery, S1–S4 passed; added traceability, report, scripts, CSV/PNG artifacts; WSL TSan/ASan non-DB gates passed | Run S5 50 seeds, DB-dependent sanitizers with actual WSL grant, and process-level X2/X3 before claiming Phase 6 exit |
| 2026-10-16 | User ran WSL TSan 34/34 with DB tests and no sanitizer report; ASan passed all but S1, which exposed 7 pending audit rows. Added TxnLog fallback resolution/reconciliation; normal S1 and `--check` now pass with I1–I5 and zero pending. S5 seeds 1–3 are recorded; script is resumable. | Rerun ASan S1, continue S5 from seed 4, then complete X2/X3 before Phase 7 |
| 2026-10-17 | Implemented Phase 7 Monitor/`--verbose`, lock-wait and latency counters, startup recovery/CLI maintenance semantics, Bash and PowerShell build/demo/check scripts, README, ADR-015, and updated traceability/reporting. Fixed Phase 5 shared-database test reset isolation. Windows build, monitor test, targeted S1, 30-test regression subset, script syntax, and real verbose CLI smoke passed; WSL TSan build/monitor passed but full TSan DB tests could not connect to `127.0.0.1:3306`. | Rerun full normal/ASan suites when practical; finish S5 seeds 17–50, X2/X3, and FR1 before final Phase 6/8 closure |
| 2026-10-20 | Added Phase 8 final technical report, presentation-ready slide outline, and examiner demo runbook. Report links architecture diagrams, test evidence, performance artifacts, exact commands, and limitations without overstating incomplete S5/X2/X3/ASan/FR1 work. | Configure MySQL credentials, rehearse the runbook, and capture a final authenticated CTest summary if required by the course |
| 2026-10-20 | Recorded ADR-016 and amended the PRD non-goal for the optional dashboard stretch. Added cpp-httplib `--serve`, localhost JSON/SSE endpoints, atomic stats/histogram/pool hooks, offline vanilla frontend with local Chart.js, guided demo buttons, endpoint tests, dashboard checklist, README instructions, and browser captures. Windows build and dashboard HTTP tests pass; DB-backed concurrency test passed in a reachable run; full TSan and idle-performance regression remain to be rerun. | Run the complete dashboard checklist, authenticate MySQL for the user's environment, run WSL TSan, and compare safe idle-dashboard throughput against Phase 6 |
