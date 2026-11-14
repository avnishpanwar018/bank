# Phase 8 Demonstration Runbook

This runbook is designed for a five-to-ten-minute live presentation. It uses
the existing CLI and tests and does not require a web UI. Run it from the
repository root.

For a guided sequence, use:

```powershell
.\scripts\phase8_demo.ps1
```

or under Bash/WSL:

```bash
bash scripts/phase8_demo.sh
```

The guided scripts stop when a required step fails and explicitly report that
the X2 process-kill step is not automated yet. Use `-SkipCheck` or
`--skip-check` when the working database is not at the opening baseline.

## 1. Before the presentation

### Prerequisites

- MySQL 8 is running.
- `.env` contains a valid TCP MySQL user and password.
- `bank` and `bank_test` exist and are seeded.
- CMake and a C++20 compiler are installed.

Check the credentials without exposing the password:

```powershell
mysql --protocol=TCP -h 127.0.0.1 -P 3306 -u $env:BANK_DB_USER -p -e "SELECT 1;"
```

If the project uses `.env` rather than process variables, use the configured
user explicitly in this check. Never paste the password into a terminal log or
presentation recording.

Build once:

```powershell
.\scripts\build.ps1
```

The Bash/WSL equivalent is:

```bash
bash scripts/build.sh
```

## 2. Scene one — startup recovery

Run:

```powershell
.\build-gcc\bank.exe --recover
```

Narrate:

> Before workers start, the application resolves unfinished audit entries. A
> normal clean run should show zero remaining PENDING rows.

Expected fields:

```text
recovery.pending_seen=0
recovery.committed=0
recovery.rolled_back=0
recovery.remaining_pending=0
```

If `Access denied` appears, stop the demo and fix `.env`; do not claim the
database portion ran.

## 3. Scene two — invariant check

Run:

```powershell
.\build-gcc\bank.exe `
  --check `
  --baseline-file results\baseline.txt
```

Or combine recovery and checking:

```powershell
.\build-gcc\bank.exe `
  --recover --check `
  --baseline-file results\baseline.txt
```

Narrate the five checks:

- **I1:** total money matches opening total plus net ledger delta.
- **I2:** no negative account balances.
- **I3:** each transaction has a valid final status.
- **I4:** no PENDING audit rows remain.
- **I5:** replaying the ledger reproduces account balances.

Expected result:

```text
I1=PASS
I2=PASS
I3=PASS
I4=PASS
I5=PASS
```

The baseline file uses integer minor units, not decimal currency values.

## 4. Scene three — safe live simulation

Run a bounded but visible workload:

```powershell
.\build-gcc\bank.exe `
  --accounts 50 `
  --clients 20 `
  --ops 100 `
  --workers 4 `
  --seed 42 `
  --verbose
```

Narrate the output:

```text
[monitor t=...] queue=.../256 active=... committed=...
 rolled_back=... total=... throughput=... tx/s
 avg_service_latency=...ms lock_waits_total=...
```

- `queue` demonstrates bounded producer/consumer back-pressure.
- `active` shows operations currently inside workers.
- `committed` and `rolled_back` show final request outcomes.
- `throughput` and `avg_service_latency` are diagnostic worker metrics.
- `lock_waits_total` is in-process account-mutex contention only.

The monitor does not query MySQL, and its thread is stopped and joined before
the simulation returns.

## 5. Scene four — unsafe lost-update demonstration

Run the recorded GoogleTest scenario:

```powershell
& "C:\Program Files\CMake\bin\ctest.exe" `
  --test-dir build-gcc `
  --output-on-failure `
  -R "UnsafeRaceTest.R2ReportsLostUpdatesAcrossTwentyRuns"
```

Point out the table:

```text
R2 run | expected cents | actual cents | lost cents
```

Narrate:

> Two unsafe workers can read the same old balance and overwrite one another's
> update. Safe mode prevents that by locking the account and using database row
> locking. This is why the unsafe mode is a teaching demonstration, not a
> production option.

Do not compare only one random run. R2 is a repeated scenario and should report
the full table when the database is configured.

## 6. Scene five — safe synchronization and deadlock avoidance

Run:

```powershell
& "C:\Program Files\CMake\bin\ctest.exe" `
  --test-dir build-gcc `
  --output-on-failure `
  -R "SynchronizationTest.R1|SynchronizationTest.R3|SynchronizationTest.D2"
```

Explain:

- R1 sends concurrent deposits to one account.
- R3 sends concurrent withdrawals and verifies no negative balance.
- D2 sends ring transfers and verifies completion without a mutex deadlock.
- Every multi-account lock request is sorted by account ID.

## 7. Scene six — full automated regression

Run after the short demonstration:

```powershell
& "C:\Program Files\CMake\bin\ctest.exe" `
  --test-dir build-gcc `
  --output-on-failure
```

The stress tests can take several minutes. A dotenv configuration test may be
skipped when a repository `.env` is present. Record the exact pass/skip/fail
summary rather than paraphrasing it.

## 8. If the database is already dirty

Do not blindly rerun `schema.sql`; existing tables produce `Table already
exists` errors. The schema scripts create the structure, but they are not a
full reset migration.

For a controlled test database reset, only do this when you are certain the
database name is `bank_test` and no other process is using it:

```powershell
mysql --protocol=TCP -h 127.0.0.1 -P 3306 -u bank_app -p `
  -e "DROP DATABASE IF EXISTS bank_test;"

Get-Content .\sql\test_schema.sql -Raw |
  mysql --protocol=TCP -h 127.0.0.1 -P 3306 -u bank_app -p

Get-Content .\sql\test_seed.sql -Raw |
  mysql --protocol=TCP -h 127.0.0.1 -P 3306 -u bank_app -p
```

The production `bank` database should not be dropped during a presentation.
The current simulator assumes accounts are already seeded; `--accounts 50`
selects IDs, it does not create them.

## 9. Evidence and limitations to state aloud

The following claims are safe:

- The project builds on Windows and WSL with CMake.
- Core functional, synchronization, retry, rollback, recovery, and monitor
  tests exist and have recorded passing runs.
- Performance charts are included, with unavailable dimensions labeled.
- The CLI monitor, startup recovery, and consistency checker are implemented.

The following must remain labeled as open:

- FR1 customer/account creation API.
- X2 process-kill recovery harness.
- X3 dropped-connection replacement seam.
- S5 seeds 17–50, because the current CSV records seeds 1–16.
- Post-fix ASan S1 confirmation.
- Current run if MySQL authentication is not configured.

## 10. Supporting artifacts

- Final report: [`FINAL_REPORT.md`](FINAL_REPORT.md)
- Slides: [`PRESENTATION.md`](PRESENTATION.md)
- Test report: [`TEST_REPORT.md`](TEST_REPORT.md)
- Traceability: [`TRACEABILITY.md`](TRACEABILITY.md)
- Performance CSV/PNG files: [`../results/`](../results/)
- Build/demo/check scripts: [`../scripts/`](../scripts/)
