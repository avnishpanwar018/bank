# Documentation Memory Pointer

The canonical living project state is maintained in the repository-root
`MEMORY.md`, as required by `AGENTS.md`. This file exists under `docs/` because
the original Phase 6 prompt refers to a `docs/MEMORY.md` path.

Phase 6/7/8 artifacts are in:

- `docs/TRACEABILITY.md`
- `docs/TEST_REPORT.md`
- `docs/FINAL_REPORT.md`
- `docs/PRESENTATION.md`
- `docs/DEMO_RUNBOOK.md`
- `docs/DASHBOARD_TEST.md`
- `docs/DECISIONS.md` (pointer to the canonical root decision log)
- `docs/img/` (dashboard captures)
- `results/`
- `scripts/`

Latest continuation (2026-10-20): the user restored MySQL; the audit-status test
passed three targeted reruns. Focused dashboard tests passed 20/20 and full serial
CTest passed **54 tests, zero failures, one expected skip** (55 discovered,
597.07 seconds). The real-browser demo helper passed and refreshed all three PNGs.
Current WSL TSan build and 20 DB-independent tests passed; the DB smoke is blocked
by MySQL rejecting the configured account from WSL. Full DB-backed TSan and a
controlled idle-overhead comparison remain open. See root `MEMORY.md`.
