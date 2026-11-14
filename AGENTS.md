# AGENTS.md — Instructions for AI Coding Agents

Read this first. Then read `MEMORY.md` (current state), `RULES.md` (hard rules) and the current phase in `PHASES.md`.

## Project
Multithreaded Bank Transaction Management System (C++20, pthreads via `std::thread`, MySQL 8/InnoDB).

## Read order
1. `MEMORY.md` — what is done / in progress
2. `PHASES.md` — current phase and exit criteria
3. `ARCHITECTURE.md` + `DESIGN.md` — before touching code
4. `RULES.md` — before every commit
5. `DECISIONS.md` — do not re-litigate; add a new ADR to change one

## Repo layout
```
bank-txn-system/
├── CMakeLists.txt
├── include/bank/      # headers
├── src/               # implementation
│   ├── main.cpp
│   ├── config.cpp
│   ├── db/            # connection_pool, db_connection
│   ├── core/          # account_manager, lock_manager, transaction_manager
│   ├── concurrency/   # bounded_queue, thread_pool
│   ├── logging/       # logger, txn_log
│   ├── sim/           # simulator, monitor
│   └── tools/         # consistency_checker, recovery
├── sql/               # schema.sql, seed.sql
├── tests/             # unit + stress + scenario tests
└── docs/              # these .md files
```

## Commands
```bash
# build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j

# DB setup
mysql -u root -p < sql/schema.sql && mysql -u root -p bank < sql/seed.sql

# run
./build/bank --accounts 50 --clients 100 --ops 10000 --workers 8 --seed 42
./build/bank --unsafe ...          # demo race conditions
./build/bank --check               # consistency checker only
./build/bank --recover             # run startup recovery only

# tests
ctest --test-dir build --output-on-failure

# sanitizers (run before finishing any concurrency change)
cmake -S . -B build-tsan -DSANITIZE=thread && cmake --build build-tsan -j && ctest --test-dir build-tsan
```

## Config (env or CLI)
`BANK_DB_HOST`, `BANK_DB_PORT`, `BANK_DB_USER`, `BANK_DB_PASS`, `BANK_DB_NAME`. Never hard-code credentials.

## Workflow for every task
1. Identify the phase and requirement ID (FR#) you are implementing
2. Write or update a test first when feasible
3. Implement the smallest change
4. Build + run tests + run ThreadSanitizer build for concurrency code
5. Update `MEMORY.md` (status, gotchas) and add an ADR in `DECISIONS.md` if a design choice was made
6. Summarize: what changed, how verified, what is next

## Definition of Done
- Compiles with `-Wall -Wextra -Werror`
- Tests pass, including TSan run for threaded code
- No new data races, no lock-order violations
- Docs updated

## Do NOT
- Skip `RULES.md` (lock ordering, no raw `new`, no sleep-based sync)
- Use `sleep()` to "fix" races
- Catch-and-ignore DB errors
- Commit secrets or generated build files
- Change the schema without updating `sql/schema.sql`, `DESIGN.md` and tests
