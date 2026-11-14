# Requirements Traceability — Phase 6/8 Audit

## Scope and source layout

The repository currently stores the source documents at the repository root
(`MEMORY.md`, `PHASES.md`, `PRD.md`, `TESTING.md`, and `RULES.md`). This report
is saved under `docs/` because Phase 6 explicitly requests that deliverable.

The implementation now contains the Phase 5 transaction/retry, recovery, and
consistency-checking layer needed by D3, X1, X4, and I5, plus the Phase 7 CLI
monitor and startup recovery orchestration. Customer/account creation,
process-kill recovery, and broken-connection replacement remain outside the
implemented scope.

The optional Phase 8 dashboard stretch is tracked separately from the core PRD
requirements under ADR-016. Its endpoint and visual checklist is in
`docs/DASHBOARD_TEST.md`; it binds only to localhost and does not change the
core transaction path.

## Functional requirements

| PRD requirement | Existing test coverage | Status | Gap / next test |
|---|---|---:|---|
| FR1 Create customer and account with non-negative initial balance | None; no creation API in `AccountManager` | **Missing** | Requires customer/account creation API and integration test. |
| FR2 Deposit | `AccountManagerIntegrationTest.F1DepositCommits`; `SynchronizationTest.R1SafeConcurrentDepositsReachExpectedBalance` | Partial | Add direct transaction/log row assertions. |
| FR3 Withdraw and reject insufficient funds | `F2WithdrawCommits`; `F3InsufficientFundsRollsBack`; `SynchronizationTest.R3ConcurrentWithdrawalsNeverOverdraw` | Pass/partial | Add negative and boundary amount cases. |
| FR4 Atomic transfer | `F4TransferIsAtomic`; `F5MissingDestinationRollsBack`; D1/D2; X1 | Pass/partial | X1 verifies rollback after a failure between debit and credit. |
| FR5 Balance check | `F9BalanceReturnsCommittedValue` | Partial | Balance is intentionally read-only under ADR-012; document that it is not recorded in `transactions`. |
| FR6 Record every operation in `transactions` and `transaction_log` | F1–F8 exercise writes indirectly; no direct row-count/status assertions | **Partial** | Add audit-row assertions for financial operations. Balance behavior is an accepted ADR-012 exception. |
| FR7 Failed operation rolls back and preserves balance | F3, F5, F6, F7, R3, X1 | Pass/partial | Additional direct audit assertions remain useful. |
| FR8 Bounded queue and worker pool | `BoundedQueueTest.*`; `ThreadPoolTest.AllSubmittedRequestsCompleteExactlyOnce`; `StressTest.S3RequestQueueSizeOneWithHundredProducers` | Pass |
| FR9 Simulator with N clients and M random operations | `StressTest.S1HundredClientsMixedOperations`; CLI smoke | Pass/partial | S1 provides the fixed-seed 100×100 workload and I1–I5 check. |
| FR10 Live monitor | `MonitorTest.EmitsSnapshotAndStopsWithoutWaitingForAnInterval`; CLI `--verbose` | **Pass/partial** | Atomic queue/worker/throughput/latency/lock-wait output is implemented; no database total-balance polling is included. |
| FR11 Startup recovery and invariant verification | `Phase5Fixture.RecoveryResolvesPendingWithoutLedgerAsCrashRollback`; `--recover` CLI | Partial | In-process recovery passes; process-kill X2 harness remains open. |
| FR12 Unsafe race demonstration | `UnsafeRaceTest.R2ReportsLostUpdatesAcrossTwentyRuns`; CLI `--unsafe` smoke | Partial | Log failure reasons and compare against safe mode. The test currently allows a run with no loss and only requires one loss across 20 runs. |
| FR13 Consistency checker | `Phase5Fixture.I5LedgerReplayMatchesBalances`; `--check` CLI | Partial | I1–I5 pass with a complete opening baseline; negative checker cases remain. |

## Non-functional requirements

| NFR | Existing evidence | Status | Gap |
|---|---|---:|---|
| Correctness: conserved transfer total and no negative balance | F1–F5, R1, R3, D1, D2, S1–S4, I5 | Pass/partial | I1–I5 are checked in stress and Phase 5 integration tests. |
| Concurrency: >=100 clients / 10,000 operations without corruption or deadlock | R1, R3, D1, D2, S1–S4 | Pass/partial | S5 50-seed execution remains pending. |
| Performance: >=500 committed tx/s | None | **Missing** | Requires benchmark harness and stable workload/resource setup. |
| Durability: committed work survives process kill | Recovery implementation; no completed X2 harness | **Missing / blocked** | Requires platform-specific kill/restart harness. |
| Observability: errors and state transitions include thread/transaction IDs | No direct test; `thread_id` is not populated by current AccountManager | **Missing** | Requires logging/transaction-manager work. |
| Portability: Linux/macOS/WSL and one-command CMake build | WSL TSan configure/build; Windows GCC/CMake build | Partial | No clean-clone cross-platform matrix. WSL DB tests require a remote MySQL grant. |

## Test-case coverage

| Category | IDs present | Status |
|---|---|---:|
| Functional | F1–F9 | Present; F6/F7 and audit assertions need strengthening. |
| Race | R1–R3 | Present; R2 is intentionally unsafe and not a sanitizer pass gate. |
| Deadlock | D1–D2 | Present; D2 needs invariant-based status assertions. |
| Deadlock/retry | D3 | Present (`Phase5Fixture` forced 1213/1205 tests) |
| Stress | S1–S4 | Present (`StressTest` suite) |
| Repeat stress | S5 | Harness present; 16/50 recorded; seeds 17–50 pending |
| Failure/recovery | X1, X4, in-process recovery | Partial; X2/X3 process/connection seams absent |
| Ledger invariant | I5 | Present (`ConsistencyChecker` and Phase5 test) |

## Baseline evidence

Command:

```powershell
& "C:\Program Files\CMake\bin\cmake.exe" --build build-gcc -j
& "C:\Program Files\CMake\bin\ctest.exe" --test-dir build-gcc --output-on-failure
```

The baseline before Phase 5 additions had 21 passes, one skipped dotenv test,
and two failures. After the Phase 5 transaction ownership refactor and stress
tests were added, the targeted Phase 5, S1–S4, and full normal suites passed.

- `SynchronizationTest.D2RingTransfersCompleteWithoutDeadlock`: one or more
  transfers rolled back, but the test requires every transfer to be committed.
- `UnsafeRaceTest.R2ReportsLostUpdatesAcrossTwentyRuns`: one request returned
  rolled back during the run, so the test stopped before recording the remaining
  run table.

The D1 workload also caused Windows MySQL error 10048 in earlier runs after
large numbers of short-lived audit connections. This is a resource-lifecycle
finding, not evidence of a lock-order deadlock. It must be fixed or explicitly
bounded before claiming the full Phase 6 suite.

## Remaining Phase 6 blockers

FR1/customer creation, X2/process-kill recovery, X3/broken connection
replacement, a fully executed S5 50-seed run, and post-fix ASan S1 confirmation
remain open. FR10's CLI monitor is implemented, with its database-independent
test recorded above. These are reported as gaps rather than hidden behind
skipped or placeholder tests.
