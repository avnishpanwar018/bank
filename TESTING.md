# TESTING.md

## 1. Strategy
Invariant-based testing: we cannot control thread interleavings, so we assert properties that must always hold.

**Invariants:** (I1) total money conserved · (I2) no negative balance · (I3) every request has exactly one final status · (I4) no unresolved PENDING after recovery · (I5) ledger replay equals final balances.

**Tools:** GoogleTest, ThreadSanitizer (`-DSANITIZE=thread`), AddressSanitizer, a dedicated test DB `bank_test` reset before each test.

## 2. Test Levels
| Level | Scope |
|-------|-------|
| Unit | Money type, BoundedQueue, LockManager ordering, config parsing |
| Integration | AccountManager + real MySQL: deposit/withdraw/transfer/rollback |
| Concurrency/Stress | Many threads, hot accounts, invariants |
| Failure/Recovery | Injected errors, `kill -9`, restart |
| Performance | Throughput/latency vs workers, pool size, isolation level |

## 3. Test Cases

### Functional
| ID | Scenario | Expected |
|----|----------|----------|
| F1 | Deposit 100 to account with 50 | Balance 150, COMMITTED |
| F2 | Withdraw 30 from 100 | Balance 70, COMMITTED |
| F3 | Withdraw 200 from 100 | ROLLED_BACK `INSUFFICIENT_FUNDS`, balance 100 |
| F4 | Transfer 40 A→B (100/100) | A=60, B=140, total 200 |
| F5 | Transfer to nonexistent account | ROLLED_BACK `ACCOUNT_NOT_FOUND`, A unchanged |
| F6 | Amount <= 0 | Rejected `INVALID_AMOUNT` |
| F7 | Transfer A→A | Rejected `SAME_ACCOUNT` |
| F8 | Duplicate `request_id` | Applied once only |
| F9 | Balance check | Returns committed balance |

### Race Conditions
| ID | Scenario | Expected |
|----|----------|----------|
| R1 | 100 threads each deposit 1 to same account (safe mode) | Balance = start + 100 |
| R2 | Same test with `--unsafe` | Lost updates → balance < expected (**demo**) |
| R3 | 50 withdraws of 10 from balance 100 concurrently | Exactly 10 succeed, final 0, never negative |

### Deadlock
| ID | Scenario | Expected |
|----|----------|----------|
| D1 | 2 threads, 10,000 opposing transfers A→B / B→A | Completes, no hang, total conserved |
| D2 | Ring: A→B, B→C, C→A concurrently | Completes |
| D3 | Force MySQL 1213 (unsafe ordering test hook) | Retry succeeds or fails cleanly after 3 attempts |

### Stress
| ID | Scenario | Expected |
|----|----------|----------|
| S1 | 100 clients × 100 ops, 50 accounts, mixed ops | I1–I3 hold |
| S2 | Hot-spot: 90% ops on 2 accounts | I1–I3 hold, no starvation (all clients finish) |
| S3 | Queue size 1, 16 workers, 100 producers | No lost/duplicated requests |
| S4 | Pool size 2, 32 workers | No deadlock, correct results |
| S5 | Repeat S1 × 50 seeds | 0 failures |

### Failure & Recovery
| ID | Scenario | Expected |
|----|----------|----------|
| X1 | Inject exception after debit, before credit | ROLLBACK, both balances unchanged |
| X2 | `kill -9` during load, restart with `--recover` | PENDING resolved, I1/I2/I4 hold |
| X3 | MySQL connection dropped mid-tx | Tx rolled back, reported `DB_ERROR`, system continues |
| X4 | Constraint violation (force negative via SQL) | DB rejects (CHECK) |

### ACID Demonstration
| Property | Test |
|----------|------|
| Atomicity | X1 |
| Consistency | I1, I2 under S1, X4 |
| Isolation | R1, R3, plus isolation-level experiment (READ COMMITTED vs REPEATABLE READ vs SERIALIZABLE) |
| Durability | X2 — committed tx still present after crash |

### Sanitizers
All of S1, R1, D1 must pass under **TSan** (no data race, no lock-order inversion) and **ASan**.

## 4. Performance Experiments (for the report)
Vary workers (1, 2, 4, 8, 16) → throughput graph · vary pool size · compare isolation levels · safe vs unsafe correctness · lock-ordering on/off deadlock rate.

## 5. Exit Criteria
All F/R/D/S/X tests pass; S5 passes 50/50; TSan clean; coverage of every FR in `PRD.md`.

## 6. Test Report Template
```
Run ID / date:
Build (Debug/TSan):
Config (accounts, clients, ops, workers, seed):
Result: PASS/FAIL
Committed / Rolled back / Retried:
Throughput (tx/s), avg latency:
Invariants I1–I5: ✔/✘
Notes:
```
