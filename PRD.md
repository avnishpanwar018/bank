# PRD — Multithreaded Bank Transaction Management System

## 1. Overview
A simplified academic banking backend that processes many deposits, withdrawals, transfers and balance checks **concurrently**, while keeping balances correct. It combines **OS concepts** (threads, mutexes, semaphores, deadlock avoidance) with **DBMS concepts** (ACID, locking, isolation, COMMIT/ROLLBACK, recovery).

## 2. Problem Statement
When many users touch the same account at once, we get race conditions, lost updates, wrong balances, deadlocks and half-finished transfers. We need a system that stays correct under concurrency and failures.

## 3. Goals
| ID | Goal |
|----|------|
| G1 | Process banking operations concurrently using multiple threads |
| G2 | Prevent race conditions with mutexes and semaphores |
| G3 | Guarantee ACID through DB transactions with COMMIT / ROLLBACK |
| G4 | Avoid / detect deadlocks in transfers |
| G5 | Keep an auditable transaction log and recover after failures |
| G6 | Provide a clear interface to run, monitor and demo the system |

## 4. Non-Goals
- No distributed deployment, sharding or replication
- No real authentication, encryption, payments or regulatory compliance
- No required web UI for the core engine; an optional read-mostly localhost
  web dashboard is allowed as a stretch/demo feature
- No multi-currency / interest / loans

## 5. Users / Personas
- **Customer (simulated):** deposits, withdraws, transfers, checks balance
- **Admin / Examiner:** starts simulation, watches monitor, reads logs, runs consistency check
- **Developer (us):** runs tests and demos race vs safe mode

## 6. Functional Requirements
| ID | Requirement | Priority |
|----|-------------|----------|
| FR1 | Create customer and account (initial balance >= 0) | Must |
| FR2 | Deposit(account, amount) | Must |
| FR3 | Withdraw(account, amount) — reject if insufficient funds | Must |
| FR4 | Transfer(from, to, amount) — atomic, all-or-nothing | Must |
| FR5 | Balance check(account) | Must |
| FR6 | Every operation recorded in `transactions` + `transaction_log` with status PENDING / COMMITTED / ROLLED_BACK | Must |
| FR7 | Failed operation triggers ROLLBACK and leaves balances unchanged | Must |
| FR8 | Worker thread pool consumes a bounded request queue | Must |
| FR9 | Load simulator spawns N clients issuing M random operations | Must |
| FR10 | Live monitor: throughput, committed/rolled-back counts, active threads, lock waits | Should |
| FR11 | Startup recovery: resolve PENDING log entries, verify invariants | Should |
| FR12 | `--unsafe` mode (no mutex, no DB locking) to demonstrate race conditions | Should |
| FR13 | Consistency checker: total money conserved, no negative balances, ledger matches balances | Must |

## 7. Non-Functional Requirements
- **Correctness:** sum of all balances is invariant under transfers; no negative balance ever
- **Concurrency:** supports >= 100 client threads / 10,000 ops without deadlock or corruption
- **Performance (target):** >= 500 committed tx/s on a laptop with local MySQL
- **Durability:** committed tx survive process kill (`kill -9`)
- **Observability:** all errors and state transitions logged with thread id and tx id
- **Portability:** Linux/macOS/WSL; one-command build with CMake

## 8. Success Metrics
- 0 invariant violations across 50 stress runs
- 0 hangs (deadlock) across stress + opposing-transfer tests
- Safe mode correct, unsafe mode visibly incorrect in the demo
- All tests in `TESTING.md` pass

## 9. Deliverables
Working application, SQL schema, synchronization module, transaction module, test results, documentation, final demo.

## 10. Assumptions & Constraints
- MySQL 8 with InnoDB; single-node
- C++20 (see `DECISIONS.md`); changeable if the course requires another language
- Academic timeline; scope is intentionally simplified

## 11. Risks
| Risk | Mitigation |
|------|-----------|
| Deadlock on opposing transfers | Global lock ordering by account id + DB deadlock retry |
| DB connection exhaustion | Connection pool guarded by counting semaphore |
| Double locking complexity (mutex + DB lock) | Clear layering rules in `RULES.md` |
| Flaky concurrency tests | Fixed seeds, invariant-based assertions, repeat runs |
