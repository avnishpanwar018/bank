# Testing, Debugging, and Integration Report

## Run metadata

- Date: 2026-10-20
- Repository: `D:/Projects/bank`
- Normal compiler: MinGW GCC 15.2.0 on Windows
- Normal build directory: `build-gcc`
- TSan build directory: `/home/hp/bank-build-tsan` under WSL2 Ubuntu
- ASan build directory: `/home/hp/bank-build-asan` under WSL2 Ubuntu
- Database: MySQL 8, `bank_test`
- Credentials: loaded from ignored `.env` / process environment; no secret is recorded here

## Commands and results

### Normal build

```powershell
& "C:\Program Files\CMake\bin\cmake.exe" --build build-gcc -j
```

Result: **PASS**.

### Normal CTest suite

```powershell
& "C:\Program Files\CMake\bin\ctest.exe" --test-dir build-gcc --output-on-failure
```

The latest complete Windows normal run is **38/38 passed**, with the dotenv
isolation test skipped because a repository `.env` already exists. This includes
the core F/R/D/S tests, the monitor test, and three dashboard HTTP tests. The
full run took approximately 721 seconds on the local Windows/MySQL setup.

### Phase 5 targeted tests

| Test | Result |
|---|---:|
| D3 forced 1213 retry succeeds on attempt 3 | PASS |
| D3 forced 1205 exhaustion returns `DB_DEADLOCK_EXHAUSTED` | PASS |
| X1 injected failure after debit rolls back both accounts | PASS |
| X4 MySQL CHECK rejects negative balance | PASS |
| I5 ledger replay equals current balances | PASS |
| Recovery resolves pending row without ledger as crash rollback | PASS |

### Phase 4 and Phase 3 tests

| Area | Result |
|---|---:|
| F1–F9 and DB smoke | PASS |
| R1 safe deposits | PASS |
| R2 unsafe lost-update demonstration | PASS in targeted/full runs |
| R3 concurrent withdrawals | PASS |
| D1 opposing transfers | PASS, no deadlock; retry layer now reduces transient failures |
| D2 ring transfers | PASS after invariant-based completion semantics |
| S3 queue capacity one | PASS |
| ConnectionPool and ThreadPool | PASS |

### Stress tests

The named tests now exist and passed individually:

| Test | Configuration | Result |
|---|---|---:|
| S1 | 100 clients × 100 operations, 50 accounts, mixed operations | PASS |
| S2 | 100 clients × 100 operations, 90% operations on two accounts | PASS |
| S3 | 100 producers, queue size 1, 16 workers, Request futures | PASS |
| S4 | 100 clients × 100 operations, pool size 2, 32 workers | PASS |

The stress tests run `ConsistencyChecker` with a 50-account opening baseline and
assert I1–I5. Observed wall times were approximately 136 s for S1, 143 s for
S2, and 130 s for S4 on this local Windows/MySQL setup.

## S5 repeat run

The repeat harness is:

```bash
scripts/repeat_stress.sh /path/to/bank-build-tsan
```

It writes `results/s5_summary.csv`, stops at the first failing seed, and records
seed, committed, rolled-back, retries, and I1–I5 fields. The WSL run recorded
seeds 1–16 as passing; the script now resumes from the last recorded seed, so
seeds 17–50 remain pending.

## Sanitizers

### TSan

WSL2 configuration and build passed:

```bash
cmake -S /mnt/d/Projects/bank -B /home/hp/bank-build-tsan \
  -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=thread
cmake --build /home/hp/bank-build-tsan -j2
```

The user's full WSL TSan run completed **34/34 tests passed**, with one expected
dotenv skip and no race or lock-order report.

Non-DB infrastructure gate:

- BoundedQueue and S3: PASS
- LockManager: PASS
- Money: PASS
- No TSan race or lock-order report

The DB-dependent TSan tests passed after the WSL gateway MySQL grant and actual
credentials were configured.

### ASan

WSL2 configuration and build passed:

```bash
cmake -S /mnt/d/Projects/bank -B /home/hp/bank-build-asan \
  -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=address
cmake --build /home/hp/bank-build-asan -j2
```

ASan passed all tests except S1. S1 completed with I1/I2/I3/I5 passing but
seven unresolved PENDING audit rows, so I4 failed. S2, S3, S4, DB smoke,
F1–F9, and the Phase 3–5 tests passed. The audit resolver now has a fallback
connection and final-status reconciliation; rerun ASan S1 to confirm the fix.

The Windows MinGW sanitizer builds are not usable: the installed toolchain has
neither `libtsan` nor `libasan`. The Windows ASan attempt also required
`-j1` because parallel compilation exhausted memory before the missing runtime
was reported.

## Bug hunt findings

| Finding | Test/fix | Status |
|---|---|---:|
| D2 required every transfer to commit although transient rollback was legal before retry support | Changed to final-status/invariant semantics; retry manager now exists | Fixed |
| R2 stopped on a single rolled-back request and did not produce its complete table | Persistent `TxnLog`/TransactionManager reduced connection churn; targeted R2 now completes | Fixed/verified targeted |
| D1 high-volume short-lived audit connections produced Windows error 10048 | TransactionManager now uses one persistent audit connection per workload | Fixed/verified in later targeted runs |
| Phase 2 account code duplicated transaction/log ownership | Added transaction-context APIs and TransactionManager ownership | Fixed |
| I5 initially failed when the test supplied only three accounts while the seeded DB had 50 | Test now captures a complete 50-account baseline; checker reports missing baseline accounts | Fixed |
| X1 had no deterministic fault seam | Added `AfterDebitBeforeCredit` fault hook and rollback regression test | Fixed |
| ASan S1 exposed seven unresolved PENDING audit rows | Added fallback audit connection and reconciliation when a primary resolution fails | Fixed pending rerun |
| X3 dropped-connection replacement | No deterministic pool invalidation/replacement seam yet | Open |
| X2 forceful child-process recovery | Recovery exists, but a process-level kill harness was not completed | Open |

## Performance artifacts

- `results/throughput_workers.csv`
- `results/throughput_pool.csv`
- `results/isolation.csv`
- `results/safe_vs_unsafe.csv`
- `results/lock_ordering.csv`
- `results/hotspot.csv`
- `results/throughput_vs_workers.png`
- `results/latency_vs_workers.png`
- `results/isolation_comparison.png`
- `results/safe_vs_unsafe_balance_error.png`
- `results/retry_rate_lock_ordering.png`

The worker throughput CSV is measured from 100-operation CLI runs repeated
three times per worker count. Latency instrumentation, independently tunable
pool size, isolation-level switching, lock-ordering-off mode, and hotspot CLI
controls are not implemented; those CSVs explicitly mark unavailable values
instead of inventing measurements. The CSV/PNG artifacts are therefore a
mixture of measured worker results and explicitly labeled unavailable
dimensions, not a claim that every requested experiment has been implemented.

## Exit-criteria status

- Traceability document: **present**, with remaining FR1/FR11/FR13 gaps and X2/X3 limitations documented; FR10 monitor work is implemented.
- F/R/D tests implemented: **mostly complete**; X2/X3/D3 were not all process-level complete.
- S1–S4: **pass** individually.
- S5 50/50: **pending**, 16/50 recorded.
- TSan safe path: **historical full 34-test WSL pass** before dashboard hardening;
  current build/20 DB-independent tests pass, but full DB-backed rerun needs a
  WSL-authorized MySQL test account (see latest continuation below).
- ASan safe path: **PENDING**, rerun S1 after audit-resolution fix.
- Results CSV/PNG: **present**, with unavailable experiments labeled.

## Phase 7 integration update

The Phase 7 integration slice adds the CLI monitor, startup recovery
orchestration, maintenance-mode CLI cleanup, and reproducible scripts. The
normal build now discovers 35 tests, including the DB-independent monitor test.

### Changes verified

| Check | Result |
|---|---:|
| CMake configure and Debug build after monitor/CLI changes | PASS |
| `MonitorTest.EmitsSnapshotAndStopsWithoutWaitingForAnInterval` | PASS |
| `bank --help` includes `--verbose`, `--check`, and `--recover` semantics | PASS |
| `bash -n` for build/demo/check/repeat-stress/benchmark scripts | PASS |
| Targeted post-change S1 run | PASS; I1–I5 all true |

One subsequent full CTest attempt was not a clean completion: S1 failed once
with I1/I5 false, a rerun of targeted S1 passed, and the full command exceeded
the local ten-minute verification window during D1. This is recorded as
verification evidence, not claimed as a full-suite pass for the Phase 7
changes. The failure appears timing/environment-sensitive and needs another
complete run before the final handoff.

The monitor reports queue occupancy, active operations, committed and rolled
back counts, throughput, average service latency, and cumulative contended
in-process account-mutex acquisitions. It does not poll MySQL or report
database row-lock waits. Remaining Phase 6 gaps are unchanged: S5 seeds 17–50,
post-fix ASan S1 confirmation, X2 process-kill recovery, X3 connection
replacement, and FR1 account creation.

## Phase 8 documentation deliverables

- [`FINAL_REPORT.md`](FINAL_REPORT.md): consolidated technical report with
  architecture, transaction lifecycle, evidence, performance artifacts, and
  limitations.
- [`PRESENTATION.md`](PRESENTATION.md): slide-ready deck outline with speaker
  notes and live commands.
- [`DEMO_RUNBOOK.md`](DEMO_RUNBOOK.md): five-to-ten-minute examiner sequence
  and database/authentication troubleshooting boundary.
- `scripts/phase8_demo.ps1` and `scripts/phase8_demo.sh`: guided safe/unsafe/
  synchronization demonstration; the unsupported X2 process-kill step is
  reported explicitly.

## Dashboard stretch verification update

The initial smoke table below predates hardening. Current results are recorded
under **Latest hardening continuation** and its MySQL-restored follow-up.

| Check | Result |
|---|---:|
| Windows Debug build with cpp-httplib and dashboard assets | PASS |
| `DashboardHttpTest.*` + `MonitorTest` | 4/4 PASS |
| 20 concurrent readers of stats/accounts/transactions during 100-client run | PASS in reachable MySQL run |
| WSL TSan dashboard static/validation tests | PASS; DB-backed concurrent reader skipped because WSL DB was unavailable |
| Safe CLI timing with dashboard server idle | INCONCLUSIVE: historic 1473.9/1223.0 ms timing smoke does not establish the <=5% target |

The historical comparison used short 100-request CLI runs on `bank_test`, with
different seeds and possible idempotent request-ID reuse. These are confounders,
not proof of a regression or of meeting the five-percent budget. A controlled
multi-run comparison with identical workloads, fresh seed data, and verified
executed-operation counts is still required.

The post-change WSL2 TSan build completed, and the dashboard static/validation
tests plus the monitor test passed without sanitizer reports. The full WSL TSan
suite was attempted but its DB-dependent tests failed immediately because WSL
could not connect to MySQL at `127.0.0.1:3306`; this is an environment
connectivity failure, not a reported TSan race.

### Latest hardening continuation (2026-10-20)

After memory improved, the single-job Windows build completed for both `bank`
and `bank_tests`:

```powershell
& "C:\Program Files\CMake\bin\cmake.exe" --build build-gcc --target bank bank_tests --parallel 1
.\build-gcc\bank_tests.exe --gtest_filter=Dashboard*
```

All **20 focused dashboard tests passed**: 4 counters, 9 DB-independent HTTP,
and 7 `bank_test` integration tests (19.523 seconds). These include Host/Origin
rejection, real abort-hook rollback, safe/race measured balances, all three session
isolation choices, manual queue execution, and concurrent readers/stop.

`scripts/dashboard_capture.py` then passed against real Chrome and `bank_test`,
refreshing all three dashboard PNGs. It observed unsafe lost updates, safe balance
match, completed opposing transfers, green I1-I5, and abort/rollback/recovery,
without uncaught JavaScript errors. Its server exited through Enter-to-stop.
JavaScript syntax and Python AST checks also passed.

The first subsequent full serial command was **not clean** (historical outage
attempt, superseded by the clean rerun below):

```powershell
& "C:\Program Files\CMake\bin\ctest.exe" --test-dir build-gcc --output-on-failure --parallel 1
```

It discovered **55 tests: 31 passed, 23 failed, 1 expected dotenv skip** in
60.75 seconds. The history-filter test first reported PENDING audit entries and
I4 failure; the next test lost its MySQL connection during reset. Later DB tests
failed with connection refused at `127.0.0.1:3306` (10061). The first audit failure
needed a targeted rerun; its cause was not established by the later outage.

`MySQL80` was confirmed stopped. Attempting to start the existing service was
denied (`Cannot open MySQL80 service on computer '.'`). The user subsequently
restored service. No MySQL configuration was changed by the agent. The earlier
out-of-memory build failure no longer blocks compilation.

WSL initially returned exit 1 without the readiness marker (an earlier launch
reported `Bash/Service/E_UNEXPECTED`), but recovered for the checks below.

### MySQL-restored verification (2026-10-20)

`MySQL80` was confirmed Running before and after the full rerun. The original
audit-status case passed **three consecutive targeted reruns**:

```powershell
.\build-gcc\bank_tests.exe --gtest_filter=DashboardDatabaseHttpTest.LimitsAndFiltersCommittedAndRolledBackTransactions --gtest_repeat=3 --gtest_break_on_failure
```

Then `bank_tests.exe --gtest_filter=Dashboard*` passed **20/20** in 18.687 seconds.
The full serial CTest command above discovered **55 tests: 54 passed, zero failed,
one expected dotenv skip**, in **597.07 seconds**. The existing `.env` causes
`ConfigTest.LoadsDotenvWithoutOverridingExistingEnvironment` to skip without
overwriting local credentials. S1-S4, R1-R3, D1-D3, rollback/recovery, and all
dashboard cases passed. No code changes were required in this follow-up. The
earlier audit failure did not reproduce with a healthy service; the original
service-outage cause is still unestablished.

Current WSL Debug TSan configure and single-job build passed:

```bash
cmake -S /mnt/d/Projects/bank -B /home/hp/bank-build-tsan -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=thread
cmake --build /home/hp/bank-build-tsan --target bank bank_tests --parallel 1
ctest --test-dir /home/hp/bank-build-tsan --output-on-failure --parallel 1 \
  -R '^(DashboardCountersTest|DashboardHttpTest|MonitorTest|MoneyTest|BoundedQueueTest|LockManagerTest)\.'
```

All **20 database-independent tests passed** in **1.75 seconds**, without a
ThreadSanitizer report. The DB smoke was then attempted with `BANK_DB_HOST` set
to the detected Windows gateway: it reached MySQL but failed with access denied
for the configured account from WSL. This is an authentication/host-grant blocker,
not a sanitizer pass for the DB path. No grants or accounts were changed; a
privately configured authorized test account is needed before the complete TSan
suite can run. The controlled <=5% idle-dashboard overhead criterion also remains
unverified; the historical timing smoke is not an adequate baseline.
