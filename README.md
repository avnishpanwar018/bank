# Multithreaded Bank Transaction Management System

This is a C++20 bank transaction simulator for Operating Systems and DBMS coursework.
It combines a bounded producer/consumer queue, a joined worker pool, ordered in-process account locks, MySQL/InnoDB transactions, a persistent transaction log, recovery, and invariant checks.

## Repository

https://github.com/avnishpanwar018/bank

## Prerequisites

- C++20 compiler with `-Wall -Wextra -Werror`
- CMake 3.20 or newer
- MySQL 8 with the Connector/C development headers and library
- A MySQL database initialized from the repository SQL files

Configure credentials without committing them:

```bash
cp .env.example .env
# edit .env; BANK_DB_* process variables override .env values
mysql -u root -p < sql/schema.sql
mysql -u root -p bank < sql/seed.sql
```

Integration tests use the separate `bank_test` database:

```bash
mysql -u root -p -e 'CREATE DATABASE bank_test;'
mysql -u root -p bank_test < sql/test_schema.sql
mysql -u root -p bank_test < sql/test_seed.sql
```

The simulator's `--accounts` value selects account IDs from the seeded database; it does not create accounts. Do not put passwords or other credentials in tracked files.

## Build and test

The reproducible scripts resolve the repository root themselves. They use `cmake` from `PATH`, or `/mnt/c/Program Files/CMake/bin/cmake.exe` under WSL; set `CMAKE_COMMAND` when needed. Native builds default to `build-gcc`; Linux/WSL builds default to `${HOME}/bank-build` to avoid CMake/FetchContent permission conflicts on mounted Windows filesystems. Set `BUILD_DIR` to override either choice.

```bash
bash scripts/build.sh
ctest --test-dir build-gcc --output-on-failure
```

On native Windows PowerShell 7, the equivalent commands are:

```powershell
.\scripts\build.ps1
ctest --test-dir build-gcc --output-on-failure
.\scripts\demo.ps1
```

Override the build directory or configuration as needed:

```bash
BUILD_DIR="$PWD/build-debug" CMAKE_BUILD_TYPE=Debug bash scripts/build.sh
```

For threaded changes, use a Linux/WSL toolchain with ThreadSanitizer:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=thread
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

## CLI Command Reference & Simulator Options

The `bank` binary accepts the following command-line flags:

| Option | Argument | Description | Default |
| :--- | :--- | :--- | :--- |
| `--accounts` | `N` | Number of accounts to sample from seeded database | `50` |
| `--clients` | `N` | Number of concurrent client producer threads | `10` |
| `--ops` | `N` | Number of operations generated per client thread | `100` |
| `--workers` | `N` | Number of worker consumer threads in thread pool | `4` |
| `--seed` | `S` | RNG seed for deterministic, repeatable transactions | `42` |
| `--verbose` | _None_ | Prints live atomic snapshots (queue, throughput, latency, lock waits) | Disabled |
| `--unsafe` | _None_ | Disables LockManager & `FOR UPDATE` to demo race conditions/lost updates | Disabled |
| `--check` | _None_ | Runs invariant checks I1–I5 (requires `--baseline-file`) | Disabled |
| `--recover` | _None_ | Reconciles orphaned `PENDING` transaction log rows | Disabled |
| `--baseline-file` | `PATH` | Path to baseline balance file (e.g. `results/baseline.txt`) | Required by `--check` |
| `--serve` | _None_ | Starts the embedded localhost web dashboard | Disabled |
| `--port` | `N` | Dashboard TCP port | `8080` |

---

### Running Natively via CLI

#### Native Windows (PowerShell)
```powershell
# 1. Safe simulation (default workload: 10 clients × 10 ops = 100 tx)
.\build-gcc\bank.exe --accounts 50 --clients 10 --ops 10 --workers 4 --seed 42 --verbose

# 2. Larger stress simulation (e.g. 50 accounts, 20 clients, 50 ops = 1,000 tx)
.\build-gcc\bank.exe --accounts 50 --clients 20 --ops 50 --workers 8 --seed 42 --verbose

# 3. Demonstrate race conditions & lost updates (unsafe mode)
.\build-gcc\bank.exe --accounts 2 --clients 10 --ops 20 --workers 4 --seed 42 --unsafe --verbose

# 4. Run startup recovery (resolves orphaned PENDING log rows)
.\build-gcc\bank.exe --recover

# 5. Run consistency checker (verifies financial invariants I1–I5)
.\build-gcc\bank.exe --check --baseline-file results\baseline.txt

# 6. Run automated test suites
.\build-gcc\bank_tests.exe --gtest_filter="MoneyTest.*"
.\build-gcc\bank_tests.exe --gtest_filter="SynchronizationTest.*"
& "C:\Program Files\CMake\bin\ctest.exe" --test-dir build-gcc --output-on-failure
```

#### Linux / WSL (Bash)
```bash
# 1. Safe simulation
./build/bank --accounts 50 --clients 20 --ops 50 --workers 4 --seed 42 --verbose

# 2. Unsafe race demo
./build/bank --accounts 2 --clients 10 --ops 20 --workers 4 --seed 42 --unsafe --verbose

# 3. Startup recovery & consistency check
./build/bank --recover
./build/bank --check --baseline-file results/baseline.txt

# 4. Run tests
ctest --test-dir build --output-on-failure
```

---

## Web Dashboard

The dashboard is an embedded visual control plane (`cpp-httplib` + vanilla JavaScript + Chart.js) binding to `127.0.0.1`.

### Start the Dashboard
```powershell
# Windows PowerShell
$env:BANK_DB_NAME = 'bank_test'
.\build-gcc\bank.exe --serve --port 8080
```
```bash
# Linux / WSL
BANK_DB_NAME=bank_test ./build/bank --serve --port 8080
```

Open **<http://127.0.0.1:8080/>** in your browser. Press **Enter** in the server terminal to stop gracefully.

### Optional Token Authentication
To require an `X-Auth-Token` header on all API endpoints:
```powershell
$env:BANK_DASHBOARD_AUTH_TOKEN = 'my-secret-token'
.\build-gcc\bank.exe --serve --port 8080
```
Test with curl:
```bash
curl http://127.0.0.1:8080/api/stats -H "X-Auth-Token: my-secret-token"
```

---

## Docker & Docker Desktop Guide

The system includes a fully containerized multi-stage Ubuntu 22.04 build (`bank-txn-system:latest`) and a configured MySQL 8 service.

### 1. Initial Build & Setup
From the repository root:
```bash
# Build the multi-stage image (compiles Linux C++20 binaries inside Ubuntu)
docker compose build

# Start MySQL and run the default 500-transaction simulation
docker compose up
```
*(If resetting existing volumes to reseed fresh tables, run `docker compose down -v` first).*

---

### 2. Running Commands with Docker Compose (Recommended)
`docker compose run` automatically manages network connections, waits for MySQL health checks, and injects `.env` configuration:

```bash
# Run a custom simulation (e.g. 50 accounts, 20 clients, 50 ops, 4 workers)
docker compose run --rm bank --accounts 50 --clients 20 --ops 50 --workers 4 --seed 42 --verbose

# Demonstrate race conditions / lost updates (unsafe mode)
docker compose run --rm bank --accounts 2 --clients 10 --ops 20 --workers 4 --seed 42 --unsafe --verbose

# Run consistency checks against database (verifies I1–I5)
docker compose run --rm bank --check --baseline-file results/baseline.txt

# Run startup crash recovery
docker compose run --rm bank --recover

# Run the complete C++ test suite inside Linux
docker compose run --rm --entrypoint /usr/local/bin/bank_tests bank

# Run filtered tests
docker compose run --rm --entrypoint /usr/local/bin/bank_tests bank --gtest_filter="SynchronizationTest.*"
```

---

### 3. Running with Standalone `docker run`
You can also run directly against the built `bank-txn-system:latest` image:

```bash
# Make sure MySQL container is running in the background
docker compose up -d mysql

# Display application CLI help
docker run --rm bank-txn-system:latest --help

# Run simulation using environment file
docker run --rm --network bank_default --env-file .env -e BANK_DB_HOST=mysql bank-txn-system:latest --accounts 20 --clients 10 --ops 20 --workers 4 --seed 42 --verbose

# Run consistency check
docker run --rm --network bank_default --env-file .env -e BANK_DB_HOST=mysql bank-txn-system:latest --check --baseline-file results/baseline.txt

# Run unit tests directly (no database required)
docker run --rm --entrypoint /usr/local/bin/bank_tests bank-txn-system:latest --gtest_filter="MoneyTest.*"
```

---

### 4. Running Inside the Docker Desktop GUI
1. **Containers View:**
   - In the Docker Desktop sidebar, click **Containers**.
   - Expand the **`bank`** group to see `bank_mysql` (Running) and `bank_app`.
   - Click the **Start / Restart (▶)** button on `bank_app` to re-execute a simulation anytime.
   - Click on `bank_app` to view live transaction throughput, latency, and log outputs.
2. **Container Terminal (Exec):**
   - Click on `bank_mysql` -> **Exec** tab to run SQL queries directly inside MySQL:
     ```bash
     mysql -u root -proot bank -e "SELECT account_id, balance FROM accounts LIMIT 10;"
     mysql -u root -proot bank -e "SELECT status, COUNT(*) FROM transactions GROUP BY status;"
     ```
3. **Images View:**
   - Go to the **Images** tab -> find **`bank-txn-system:latest`** -> click **Run**.
   - Under **Optional settings**, set Network to **`bank_default`**, add `BANK_DB_HOST=mysql`, and enter desired command arguments.

---

### 5. Stopping and Resetting Containers
```bash
# Stop containers (preserves database data)
docker compose down

# Stop containers and wipe database volume (clean re-seed on next boot)
docker compose down -v
```

## Project layout

- `include/bank/`, `src/`: production headers and implementation
- `tests/`: unit, integration, stress, and recovery tests
- `sql/`: schema and seed data
- `scripts/`: build, demo, check, stress, and benchmark helpers
- `scripts/phase8_demo.ps1` / `scripts/phase8_demo.sh`: guided presentation sequence
- `web/`: offline static dashboard frontend
- `docs/DASHBOARD_TEST.md`: endpoint/button/chart checklist
- `docs/TRACEABILITY.md`: requirement coverage and known gaps
- `docs/TEST_REPORT.md`: recorded verification and sanitizer status
- `docs/FINAL_REPORT.md`: final technical report
- `docs/PRESENTATION.md`: slide-ready presentation outline
- `docs/DEMO_RUNBOOK.md`: examiner-facing demonstration sequence
- `MEMORY.md`, `PHASES.md`, `RULES.md`, `DECISIONS.md`: project state, roadmap, invariants, and ADRs

Known remaining work is documented rather than hidden: full S5 50-seed execution, post-fix ASan S1 confirmation, process-level X2 recovery, X3 connection replacement, and FR1 account creation.
