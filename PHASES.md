# PHASES.md — Roadmap (maps to project milestones)

Rule of thumb: finish a phase's **exit criteria** before moving on. Update `MEMORY.md` after each phase.

## Phase 0 — Requirement Analysis
- Finalize `PRD.md`, confirm language/stack with instructor, set up repo, install MySQL + CMake + GoogleTest
- **Exit:** PRD approved, `hello world` builds and connects to MySQL

## Phase 1 — System & Database Design
- ER diagram, `schema.sql`, class diagram, architecture + sequence diagrams (`ARCHITECTURE.md`, `DESIGN.md`)
- **Exit:** schema reviewed, all components and interfaces defined

## Phase 2 — Database Implementation
- Create tables, constraints, indexes, seed data (e.g. 50 accounts × ₹100,000)
- `DbConnection` RAII wrapper, prepared statements, `AccountManager` single-threaded CRUD (deposit/withdraw/transfer/balance)
- **Exit:** F1–F9 pass single-threaded

## Phase 3 — Multithreading Implementation
- `BoundedQueue`, `ThreadPool`, `Simulator`, `ConnectionPool` — **first without locks** to observe races
- Add `--unsafe` baseline
- **Exit:** R2 reproduces lost updates (this is your demo evidence)

## Phase 4 — Synchronization
- `LockManager` with ordered per-account mutexes, queue semaphores, pool semaphore
- Deadlock test with and without ordering
- **Exit:** R1, R3, D1, D2 pass; TSan clean

## Phase 5 — Transaction Management
- `TransactionManager`: BEGIN/COMMIT/ROLLBACK, `FOR UPDATE`, retry on 1213/1205, `transaction_log` (PENDING → final), idempotency
- `Recovery` and `ConsistencyChecker`
- **Exit:** F1–F9, X1–X4, S1–S4 pass; ACID demonstrated

## Phase 6 — Testing & Debugging
- Full `TESTING.md` suite, 50-seed repeat, ASan + TSan, performance experiments and graphs
- **Exit:** all exit criteria in `TESTING.md` met

## Phase 7 — System Integration
- `Monitor`, CLI polish, config, one-command build/run script, README
- **Exit:** clean clone → build → run demo works end to end

## Phase 8 — Documentation & Presentation
- Final report (use these docs as base), diagrams, test results, performance graphs
- Demo script: (1) unsafe → wrong balance, (2) safe → correct, (3) opposing transfers no deadlock, (4) crash + recovery
- **Exit:** report + slides + rehearsed demo

### Current Phase 8 status
- **Deliverables complete:** `docs/FINAL_REPORT.md`, `docs/PRESENTATION.md`,
  `docs/DEMO_RUNBOOK.md`, Mermaid architecture/transaction/deadlock/race
  diagrams, existing CSV/PNG performance artifacts, and guided
  `scripts/phase8_demo.ps1` / `scripts/phase8_demo.sh`.
- **Evidence boundary:** the guided demo explicitly reports X2 as not
  automated. The current runtime rehearsal also requires valid MySQL
  credentials; incomplete S5, ASan, X2, X3, and FR1 work remains documented in
  the final report instead of being claimed complete.

## Stretch Goals (only if time allows)
- **Optional localhost web dashboard:** implemented as a read-mostly demo stretch;
  see `docs/DASHBOARD_TEST.md`, `web/`, and ADR-016.
- Deadlock detection via wait-for graph, read-write locks for balance checks,
  MVCC isolation comparison, account statements, interest batch job

## Suggested Timeline (adjust to deadline)
| Week | Phases |
|------|--------|
| 1 | 0–1 |
| 2 | 2–3 |
| 3 | 4–5 |
| 4 | 6–8 |
