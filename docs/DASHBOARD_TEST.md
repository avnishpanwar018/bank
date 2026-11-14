# Optional Dashboard Test Checklist

This checklist covers the localhost-only dashboard stretch feature. Record the
date, browser, build directory, MySQL database, and exact command used before a
manual run. Do not record credentials.

## Recorded continuation (2026-10-20)

- Windows `build-gcc` single-job build: passed.
- `bank_tests.exe --gtest_filter=Dashboard*`: **20/20 passed**.
- Real headless Chrome, `bank_test`, `python scripts/dashboard_capture.py`:
  race/safe/opposing-transfer/check/abort-recovery flows passed, no uncaught JS
  errors, all three PNGs refreshed. The check screenshot shows green I1-I5.
- After the user restored MySQL, the audit-status test passed three targeted
  reruns, dashboard tests passed 20/20 again, and full serial CTest passed
  **54 tests, zero failures, one expected skip** (55 discovered, 597.07 seconds).
  MySQL remained Running. The earlier outage run is history in `TEST_REPORT.md`.
- Current WSL TSan build and **20 DB-independent tests passed**. The gateway DB
  smoke reached MySQL but the configured account was denied from WSL; full
  DB-backed TSan needs authorized account/host configuration.
- Controlled <=5% idle-overhead measurement remains unverified.

The checklist below remains a repeatable manual checklist, not a blanket claim
that every visual/accessibility/reconnect check was completed by that browser run.

## Environment

- [x] MySQL 8 is running and configured credentials work over TCP for native tests.
- [ ] `bank` and `bank_test` are seeded.
- [ ] `scripts/build.ps1` or `scripts/build.sh` completes.
- [ ] Server starts with `bank --serve --port 8080`.
- [ ] Browser opens only `http://127.0.0.1:8080/`.
- [ ] No external network dependency is required after build; Chart.js is local.

## HTTP endpoint checks

| Endpoint | Expected | Result / evidence |
|---|---|---|
| `GET /` | 200, dashboard HTML | [ ] |
| `GET /api/stats` | 200, JSON with counters, latency, queue, pool, balance, mode | [ ] |
| `GET /api/stream` | SSE event at about 500 ms intervals | [ ] |
| `GET /api/accounts?page=1&limit=50` | 200, paginated account/owner/balance rows | [ ] |
| `GET /api/transactions?limit=50` | 200, latest transactions and log status | [ ] |
| `GET /api/transactions?limit=50&status=ROLLED_BACK` | 200, filtered rows | [ ] |
| `POST /api/simulate` valid payload | 202, background run starts | [ ] |
| `POST /api/simulate` while active | 409 JSON error | [ ] |
| `POST /api/simulate` invalid range/type | 400 JSON error | [ ] |
| `POST /api/simulate/stop` | 200 when active; 409 when idle | [ ] |
| `POST /api/tx` valid request | 200 result; same queue/TransactionManager path | [ ] |
| `POST /api/tx` amount <= 0 / invalid type | 400 JSON error | [ ] |
| `POST /api/check` while idle | I1–I5 JSON result | [ ] |
| `POST /api/check` while running | 409 JSON error | [ ] |
| `POST /api/recover` while idle | recovery counts and remaining pending | [ ] |
| `POST /api/reset` without `confirm=true` | 400; no mutation | [ ] |
| `POST /api/reset` with `confirm=true` | 200; development DB reseeded | [ ] |

Example valid simulation body:

```json
{
  "accounts": 50,
  "clients": 20,
  "ops": 100,
  "workers": 4,
  "seed": 42,
  "unsafe": false,
  "isolation": "REPEATABLE_READ",
  "hotspot_ratio": 0
}
```

## Visual checks

- [ ] Top status changes Idle → Running → Idle.
- [ ] Unsafe mode is red and labeled `UNSAFE`.
- [ ] Invariant badge starts unknown and changes after `/api/check`.
- [ ] Throughput chart receives SSE points without page reload.
- [ ] Committed/rolled-back/retry chart updates.
- [ ] Rollback-reason doughnut updates from stats counters.
- [ ] Queue-depth/active-workers chart updates.
- [ ] Accounts table loads and changed balances flash briefly.
- [ ] Transactions table loads, color-codes status, and filters by status.
- [ ] SSE disconnect banner appears when the server is stopped and clears after
      reconnect.
- [ ] Malformed SSE JSON does not crash the page.
- [ ] No raw server string is inserted with `innerHTML`.
- [ ] Keyboard focus is visible on controls and buttons.
- [ ] Layout remains usable at narrow/mobile viewport width.

## Demo buttons

- [x] **Race Condition Demo** starts an unsafe deposit workload and displays
      expected account-1 balance, actual balance, and lost updates.
- [x] **Safe Run** starts the same deposit scenario with ordered locking.
      Expected and actual balances match.
- [x] **Deadlock Demo** starts opposing transfers and reaches Idle without hang.
- [x] **Run Consistency Check** shows I1–I5.
- [x] **Crash + Recover** injects an exception after debit, before credit via
      the existing TransactionManager hook; both balances survive rollback and
      recovery/check reports are shown. Recovery may see zero pending entries
      because exception handling already resolved them. This is not an OS kill.

## Concurrency and sanitizer checks

- [x] Run the HTTP concurrency GoogleTest while MySQL is reachable (full CTest):

  ```powershell
  & "C:\Program Files\CMake\bin\ctest.exe" `
    --test-dir build-gcc --output-on-failure `
    -R DashboardDatabaseHttpTest.ConcurrentReadEndpoints
  ```

- [x] During a 100-client run, use 20 concurrent readers against `/api/stats`
      and `/api/accounts`.
- [x] Run the complete normal test suite (54 passed, zero failures, one skip).
- [ ] Run the WSL2 ThreadSanitizer build and complete suite.
- [ ] Compare safe idle-dashboard throughput against the Phase 6 baseline;
      accept no more than approximately 5% regression for the same workload.

## Screenshots

Keep three screenshots with the final report:

- `docs/img/dashboard-overview.png`
- `docs/img/dashboard-race.png`
- `docs/img/dashboard-check.png`

The PNGs were refreshed by `scripts/dashboard_capture.py` against real Chrome
and `bank_test`: overview, unsafe lost-update result, and a green I1-I5 check
after a reset, safe deposit run, and opposing transfers. The helper requires
Chromium and the optional Python `websockets` package; it reseeds `bank_test`,
so do not run it alongside integration tests. It does not complete every manual
check above. MySQL service availability was restored for the clean native rerun.

## Approximately 60-second live demo

Prepare outside the timed demonstration: start MySQL, build the executable, and
launch from the repository root in a separate terminal:

```powershell
$env:BANK_DB_NAME = 'bank_test'
.\build-gcc\bank.exe --serve --port 8080
```

Open <http://127.0.0.1:8080/>. Use `bank_test` only: reset destroys its existing
data, and no integration tests should run concurrently. Each step waits for a
completed result/Idle, not a fixed sleep; actual duration depends on MySQL speed.

| Approx. time | Action and explanation |
|---|---|
| 0-5 s | **Reset + reseed**, confirm, and wait for reset success. |
| 5-15 s | **Race condition**. Show expected vs actual account-1 minor units and measured lost updates. If none occur, say so and rerun; races are interleaving-dependent. |
| 15-20 s | Reset again to remove the deliberately unsafe state. |
| 20-30 s | **Safe run**. Wait for expected and actual balances to match. |
| 30-40 s | **Deadlock demo**. Opposing transfers complete using ascending lock order. |
| 40-45 s | **Run consistency check**. Show green I1-I5. |
| 45-60 s | **Crash + recover**. Explain the after-debit exception, rollback, recovery report, and invariant results. This is not a process kill. |

Afterward press Enter in the server terminal for joined shutdown. The three
screenshots are a fallback if live infrastructure is unavailable, not a substitute
for asserting the live run passed.
