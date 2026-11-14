# RULES.md — Non-Negotiable Project Rules

## Concurrency (OS layer)
1. **Lock ordering:** to lock multiple accounts, always go through `LockManager::lock({ids...})` — it sorts ascending. Never lock two account mutexes manually.
2. **RAII only:** no bare `mutex.lock()/unlock()`. Use `std::scoped_lock` / `std::unique_lock` / `ScopedLocks`.
3. **Keep critical sections tiny:** only read-validate-write. No logging I/O, no `sleep`, no user input inside.
4. **No sleep-based synchronization.** Use mutex, condition variable or semaphore.
5. **No shared mutable state without protection:** use mutex or `std::atomic`. Document which mutex guards which data (`// guarded by: ...`).
6. **Semaphores:** queue uses `empty`/`full` counting semaphores + a mutex; connection pool uses a counting semaphore sized to pool size. Always release in a RAII guard.
7. **Threads are joined:** no detached threads. Shutdown via `close()` on queue + `join()`.
8. **Every concurrency change must pass the ThreadSanitizer build.**

## Transactions (DBMS layer)
9. **One request = one DB transaction.** `START TRANSACTION` → work → `COMMIT`, or `ROLLBACK` on any error.
10. **Lock rows in the same order as mutexes** (`ORDER BY account_id ... FOR UPDATE`).
11. **Never trust in-memory balance** — read it from DB inside the transaction.
12. **Retry only on** MySQL 1213 (deadlock) and 1205 (lock wait timeout); max 3 attempts, jittered backoff. Never retry validation errors.
13. **Log before, resolve after:** write `PENDING` (separate connection, autocommit) before the transaction; write `COMMITTED` / `ROLLED_BACK` after.
14. **Idempotency:** every request has a unique `request_id`; duplicates must not apply twice.
15. **Use prepared statements** for all SQL. No string-concatenated SQL.
16. **Money = integer minor units in C++, DECIMAL in SQL.** No floating point.

## Code Quality
17. C++20, `-Wall -Wextra -Werror`, no warnings.
18. No raw `new`/`delete`; use `std::unique_ptr`, `std::vector`, RAII wrappers around `MYSQL*`.
19. Functions < 50 lines where reasonable; one responsibility per class.
20. Naming: `snake_case` for functions/variables/files, `PascalCase` for types, `kConstant` for constants.
21. All DB / system calls check return codes and surface errors via `Result` or exceptions — never ignored.
22. No secrets in code or repo; credentials from env vars.

## Testing
23. Each FR has at least one test (see `TESTING.md`).
24. Stress tests use fixed seeds and assert **invariants**, not exact interleavings.
25. A bug fix starts with a failing test.

## Process
26. Small commits: `type(scope): message` — e.g. `feat(lock): ordered multi-account locking`.
27. Update `MEMORY.md` at the end of each work session.
28. Design changes need a new entry in `DECISIONS.md`.
29. Don't expand scope beyond `PRD.md` non-goals without a decision entry.
