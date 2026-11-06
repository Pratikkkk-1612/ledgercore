# LedgerCore engineering journal

## 2026-11-06 — Baseline validation before further development

### Objective and scope

Inspect the existing implementation, compare the working tree with Git state,
attempt a fresh build and validation, and separate implemented behavior from
verified behavior. No application, schema, or feature changes were made.

### Repository evidence

- Branch: `main`.
- HEAD: `3691a6738c3cfd04668a004a491a5d08ea5095a3`.
- Locally cached `origin/main`: the same commit.
- Configured remote: `https://github.com/Pratikkkk-1612/ledgercore.git`.
- Before this journal was added, `git status --short` was empty and
  `git diff --exit-code HEAD` / `git diff --exit-code origin/main` passed.
- Eight tracked files: `.env.example`, `.gitignore`, `CMakeLists.txt`,
  `README.md`, two migrations, `src/main.cpp`, and one SQL test script.
- Build output and `passwords.txt` are ignored local files. Credential contents
  were not read. `docs/` contained no journal before this review.
- Live `git ls-remote origin refs/heads/main` failed with
  `getaddrinfo() thread failed to start`. Web retrieval and browser access also
  failed. Consequently, equality with GitHub's CURRENT branch is unverified;
  equality with the locally cached remote-tracking commit is verified.
- This journal is a new local, uncommitted file; nothing was pushed.

### What is implemented

| Roadmap phase | Implementation inspected | Validation status in this review |
| --- | --- | --- |
| 0 — Requirements | User-supplied roadmap and a short repository README | Detailed requirements and acceptance criteria are not yet recorded in repository documentation |
| 1 — Build baseline | C++20 executable, CMake, PostgreSQL libpq linkage | Fresh configure and Debug build passed on this Windows machine |
| 2 — Database model | Users, accounts, transactions, journal entries; identities, foreign keys, uniqueness, nonnegative account balances, nonzero entries, indexes | SQL inspected; application of migrations to a live database not verified |
| 3 — Double-entry invariant | Initially deferred constraint trigger checks entry sum, including old transaction on entry reassignment; parent-row locking | Code inspected; SQL scenarios exist, but were not executed in this review |
| 4 — Atomic transfer | Integer-paise input, decimal formatting, parameterized SQL, conditional debit, credit, transaction and paired entries, commit/rollback, failure injection | Compiles; transfer outcomes and database state not runtime-verified |
| 5 — Concurrency | Conditional UPDATE and trigger locking are present | No concurrent experiment or retry policy found; phase not established as complete |
| 6–12 | API, idempotency/Redis, outbox, Kafka, broader reliability tests, deployment, performance | No implementation found in tracked files |

### Measured checks

1. `cmake -S . -B build/audit` — passed. MSVC 19.51.36257.0,
   Visual Studio 18 2026 generator, PostgreSQL 18.6 discovered.
2. `cmake --build build/audit --config Debug` — passed; produced
   `build/audit/Debug/ledgercore.exe`.
3. `ctest --test-dir build/audit -C Debug --output-on-failure` — reported
   `No tests were found!!!`. Its zero exit status does NOT mean behavior was tested.
4. A noninteractive `psql` read-only connection to `127.0.0.1:5432`, database
   `ledgercore`, user `ledgercore`, failed with socket permission error 10013.
   This establishes an access blocker, not that PostgreSQL is stopped or broken.
5. No live database contents were read or changed. The hard-coded transfer
   executable and SQL test script were not run against existing accounts.

### Findings from code inspection

1. **Currency correctness is not enforced.** `transfer()` always records INR,
   while debit/credit UPDATEs filter by account ID without checking currency.
   Schema foreign keys do not require journal accounts to match transaction
   currency. A non-INR account could therefore participate in an INR transfer.
   This is a source-level finding, not a reproduced database experiment.
2. **Rollback-test success can be false.** The CLI returns exit code zero for
   any `false` transfer result in rollback mode. Insufficient funds, a duplicate
   reference, or another early failure can bypass injection and still be called
   a completed rollback test. No before/after state assertion is present.
   Injection also assumes user ID 999999 does not exist.
3. **Balanced entries do not guarantee balance reconciliation.** The trigger
   checks journal sums only. Direct changes to account balances or balanced
   journal mutations are not coupled by a database invariant. Empty transaction
   headers are also allowed, and removing all entries yields a zero sum.
   Define the opening-balance policy, permitted write paths, and whether posted
   records must be immutable before claiming stronger accounting guarantees.
4. **SQL tests are manual examples, not a repeatable asserted suite.** They
   assume accounts 1 and 2 exist, use fixed references, commit the valid case,
   and display results without programmatically asserting expected outcomes.
5. **Build/configuration is machine-specific.** PostgreSQL include/library
   paths are hard-coded for Windows. `main()` hard-codes host, port, database,
   and user; it does not load `.env` or consume the example's `POSTGRES_*`
   variables. Standard libpq credential mechanisms are separate from this.
6. **Documentation overstates the current stack by implication.** README lists
   Redis, Kafka, Docker, Testcontainers, AWS, Terraform, and React without
   separating plans from implementation. Existing phase2/phase3 labels also
   differ from the supplied roadmap's phases 3/4.
7. **Concurrency needs its own evidence.** Opposing transfers update source
   then destination, so opposite lock order can cause deadlocks. No dedicated
   concurrency test or retry classification was found. Parent locking in the
   balancing trigger alone is not evidence for all isolation levels or schedules.

### Decisions and next validation steps

- Keep this audit separate from feature implementation and preserve the
  phase-by-phase learning approach.
- Finish live GitHub verification from an environment with working access.
- Use a disposable database with known seed accounts for runtime checks.
- Assert both results and database evidence for successful transfers,
  insufficient funds, invalid accounts, duplicate references, injected failure,
  and rejected unbalanced journals. Include currency and reconciliation cases.
- Repair rollback-test false positives and settle the currency/reconciliation
  contracts before declaring the earlier phases validated.
- Then conduct controlled concurrency experiments in phase 5; do not introduce
  later infrastructure to compensate for unresolved accounting assumptions.

### Learning checkpoint

Begin with the purpose of the four tables, an explanation of a transfer's
balance and journal effects, and the distinction between atomicity and a
balanced journal. Follow with deferred checks, parameterized SQL, failure
evidence, duplicate requests, and concurrency after the initial answers.
No knowledge score has been assigned: learner responses are still pending.

## 2026-11-06 — Phase 2 live schema inspection (user-run evidence)

The user connected through their own psql 18.6 terminal and supplied output
from a read-only inspection. This evidence is user-provided; the agent's
direct database connection remains blocked.

- `current_database()` and `current_user` both returned `ledgercore`.
- Definitions of the four public tables match the columns, types, defaults,
  primary keys, unique constraints, checks, foreign keys, and indexes in
  migration 001 inspected during the baseline audit.
- `journal_entries` has `trg_journal_entries_balanced`, an AFTER INSERT,
  DELETE, or UPDATE constraint trigger, DEFERRABLE INITIALLY DEFERRED,
  invoking `enforce_balanced_transaction()`. The live function body has not
  yet been inspected, and trigger behavior has not yet been tested.
- Currency grouping returned two INR accounts and seven INR transactions,
  with no other currency groups. Existing data uses INR; neither table has
  an INR-only CHECK constraint in the supplied definitions.
- Output includes the issued COMMIT command but not its server response.
  No writes were included in the supplied inspection commands.

Verdict: structural inspection passed for the displayed definitions;
Phase 2 behavioral validation remains pending. Counts and constraint
definitions do not establish transfer correctness or journal reconciliation.

Next step: create and verify a separate validation database, apply the
existing migrations there, and test each basic constraint against controlled
fixtures. If database creation is denied or the chosen name already exists,
stop and inspect that result; do not drop databases or change role privileges.

### Validation database setup — user-run results

- First CREATE DATABASE attempt failed because the earlier read-only
  transaction was still open. ROLLBACK ended that failed transaction.
- The next attempt, outside a transaction, failed because `ledgercore`
  lacks CREATEDB permission. No role privileges were changed.
- The user connected as `postgres` and successfully executed
  `CREATE DATABASE ledgercore_validation_20261106 OWNER ledgercore;`.
- After reconnecting as the application role, the user's SELECT output
  confirmed database `ledgercore_validation_20261106` and user `ledgercore`.
- This confirms isolated database creation and access, not schema installation
  or behavioral correctness. Applying migrations 001 and 002 is the next step.

Justification: administrative provisioning does not require granting the
application role server-wide database creation privileges. Existing ledger
data remains separate from the upcoming validation fixtures.

### Migration installation - user-run results

- Migration 001 ran inside an explicit transaction: four CREATE TABLE and
  two CREATE INDEX messages, followed by COMMIT.
- Migration 002 reported BEGIN, CREATE FUNCTION, DROP TRIGGER,
  CREATE TRIGGER, and COMMIT. Its first-run missing-trigger NOTICE is expected
  because it uses DROP TRIGGER IF EXISTS.
- The table listing showed all four public tables owned by `ledgercore`.
- Explicit counts returned zero rows in users, accounts, transactions, and
  journal_entries.
- The supplied output ends at the `\d public.journal_entries` command;
  its resulting description was not included. Trigger creation succeeded
  according to the migration output, but that follow-up display is not evidence
  yet supplied.

Verdict: both existing migrations installed successfully in the separate
validation database. Phase 2 behavioral tests have not yet run. Next test:
accept a valid user/account pair, reject an account with a nonexistent owner
through fk_accounts_user, recover using a savepoint, and roll back fixtures.

### Phase 2 test 1 - account ownership and default balance: PASS

Evidence: user-supplied psql output from `ledgercore_validation_20261106`.

- Follow-up journal table description confirms the expected indexes, foreign
  keys, nonzero-amount check, and attached initially deferred balancing trigger.
- Inserted a test user and captured its generated ID with `\gset`.
- Inserting an INR account for that user succeeded and returned balance 0.00
  when the INSERT omitted the balance column.
- Inserting an account for nonexistent user -1 failed specifically on
  `fk_accounts_user`, with a missing-users-row detail.
- ROLLBACK TO SAVEPOINT recovered from that expected error. The single valid
  account remained visible with its original 0.00 balance.
- Full ROLLBACK removed the fixtures; the final user and account counts were
  both zero.

Justification: observed both valid acceptance and rejection by the intended
constraint, rather than treating any SQL failure as a passing test. This
validates the tested account-ownership INSERT path and default balance; it
does not validate the C++ transfer engine, journal balancing, or concurrency.
Next: test the nonnegative-balance check on INSERT and UPDATE, plus acceptance
of the zero boundary. Phase 2 remains in progress.

### Phase 2 test 2 - nonnegative balances: PASS

Evidence: user-supplied psql output from the validation database.

- INSERT with balance -0.01 was rejected by `chk_account_balance`.
- After savepoint recovery, a fixture account with balance 100.00 was accepted.
- UPDATE subtracting 100.01 was rejected by the same named constraint.
- After recovery, SELECT confirmed that the balance was still 100.00.
- UPDATE subtracting exactly 100.00 succeeded, returning balance 0.00 and
  UPDATE 1. This validates acceptance at the lower boundary.
- Full ROLLBACK returned both user and account counts to zero.

Justification: the check was exercised on INSERT and UPDATE, with negative
rejection and zero acceptance. The opening 100.00 is a test fixture, not a
journal-backed deposit. These observations do not establish transfer atomicity,
reconciliation, concurrency safety, or rejection of special numeric values.
Phase 2 remains in progress; next are account and transaction-reference
uniqueness tests.

### Phase 2 test 3 - uniqueness: partial PASS

Evidence: user-supplied psql output from the validation database.

- Two different users each received an INR account successfully.
- A second INR account for the first user was rejected specifically by
  `uq_account_user_currency`.
- Savepoint recovery succeeded; grouped counts showed one INR account per user.
- Transaction-reference setup and the duplicate-reference INSERT were absent
  from the supplied execution. ROLLBACK TO SAVEPOINT before_duplicate_reference
  failed because that savepoint did not exist. The following SELECT failed
  because the transaction was aborted; neither error tests reference uniqueness.
- Full ROLLBACK recovered the session. All four final table counts were zero.

Verdict: account composite uniqueness passed the tested INSERT scenarios;
transaction-reference uniqueness remains untested. This was an execution-order
error, not evidence of a schema defect. Rerun only the reference test, including
its BEGIN, original INSERT, and SAVEPOINT before attempting the duplicate.

### Phase 2 test 3 follow-up - transaction-reference uniqueness: PASS

The user's corrected execution included BEGIN, the original transaction INSERT,
and SAVEPOINT before_duplicate_reference. Inserting the same reference again
failed specifically on `transactions_reference_key`. After savepoint recovery,
the grouped count for P2-UNIQUE-001 was exactly one. Full ROLLBACK succeeded,
and the final transactions count was zero.

Together with the prior account-uniqueness results, the planned INSERT
uniqueness scenarios now pass. This establishes duplicate-reference rejection,
not full request idempotency, concurrent duplicate handling, or transfer
rollback behavior. Next: test journal-entry foreign keys while keeping the
other reference valid, so each expected failure has one intended cause.

### Phase 2 test 4 - journal-entry references: PASS

Evidence: complete user-supplied psql output from the validation database.

- Created a temporary user, INR account, and transaction header; captured all
  generated IDs with psql variables.
- An entry with transaction_id -1 and a valid account was rejected specifically
  by `fk_journal_transaction`.
- After savepoint recovery, an entry with a valid transaction and account_id -1
  was rejected specifically by `fk_journal_account`.
- After recovery, journal_count was zero.
- Full ROLLBACK succeeded and all four table counts were zero.

Justification: each attempted entry violated one intended reference while the
other reference was valid, identifying the constraint responsible for each
rejection. This validates the tested INSERT reference checks, not the deferred
sum check. Phase 2 remains in progress. Next: zero and NULL journal amounts,
using valid references to isolate the amount constraints.

### Phase 2 test 5 - zero and missing journal amounts: PASS

Evidence: user-supplied psql output from the validation database.

- Created valid temporary user, account, and transaction references.
- INSERT with amount 0.00 failed on `chk_journal_amount_nonzero`.
- After savepoint recovery, INSERT with amount NULL failed on the amount
  column's not-null constraint.
- The user used full ROLLBACK immediately after the NULL rejection rather
  than returning to its savepoint. This valid cleanup ended the failed
  transaction. All four final table counts were zero.
- The intermediate journal-count query was not run for this test; the named
  rejection errors and final cleanup counts were observed.

Justification: zero and missing amounts are separate invalid inputs enforced
by different constraints. The tests isolate those checks with valid references.
These tested INSERT paths pass; broader input validation and the outstanding
INR-only restriction are not thereby established. Next: reproduce whether
non-INR account and transaction rows are currently accepted, then address
the currency restriction before moving to phase 3.

### Phase 2 test 6 - INR-only requirement: gap reproduced

Evidence: user-supplied psql output from the validation database.

- One test user received both INR and USD accounts successfully.
- A USD transaction header was also accepted.
- Full ROLLBACK succeeded; all four table counts returned to zero.

Verdict: the current schema does not enforce the intended INR-only scope.
Acceptance is a reproduced requirements gap, not a passing restriction test.

### Currency restriction implementation - prepared, not runtime-validated

Added `migrations/003_enforce_inr_currency.sql` with CHECK (currency = 'INR')
on public.accounts and public.transactions, named chk_account_currency_inr
and chk_transaction_currency_inr. Existing currency NOT NULL constraints
remain in place. The migration wraps both additions in one transaction and
validates existing rows; incompatible records cause failure rather than being
converted or relabeled. This is an apply-once migration like the initial schema.

Decision: database CHECK constraints enforce the single-currency rule for
INSERT and UPDATE across normal application and direct SQL writes. A C++-only
check would leave other write paths unprotected. No conversion or multi-currency
transfer support is being introduced. Existing migrations are preserved so
already-created databases can receive the change through a new migration.

Validation status: source inspected; not applied to either database by the
agent. Next the user will apply migration 003 to the validation database and
inspect the installed constraints, then test INR acceptance and non-INR
rejection on both INSERT and UPDATE. The original ledgercore database has not
received this migration; no runtime currency-fix success is claimed yet.

### Migration 003 installation - user evidence confirmed

The user supplied BEGIN, two ALTER TABLE results, and COMMIT from the validation
database. A pg_constraint query returned both INR CHECK constraints with
convalidated = true. This confirms installation; behavioral coverage is now
included in the consolidated phase-2 suite below.

## Validation workflow correction - one suite per phase

The user explicitly requested consolidated validation rather than repeated
manual test fragments. From this point:

| Phase | Validation entry point | Status |
| --- | --- | --- |
| 1 - Build | Existing fresh CMake configure/build evidence | Passed on this machine |
| 2 - Database model | tests/validate_phase2.sql | Created; SQL body tested in isolated PostgreSQL; user database run pending |
| 3 - Double entry | One dedicated suite with commit/failure and mutation cases | Planned, not implemented or passed |
| 4 - Atomic transfers | One suite exercising the actual C++ transfer function and checking persisted state | Planned, not implemented or passed |

Explain each suite's scope before execution, review its single summary, fix
observed failures within that phase, and rerun the same suite. No later phase
is certified by an earlier suite. The legacy phase2_double_entry_tests.sql
name predates the current roadmap; it is not the consolidated phase-2 runner.

### Consolidated Phase 2 implementation and measured results

Added tests/validate_phase2.sql: 58 checks covering valid fixtures/defaults,
foreign keys and referenced-parent deletion, uniqueness, negative/zero money,
currency INSERT/UPDATE paths, required non-identity columns, exact decimals,
numeric limits, special numeric values, and preservation of fixture state.
Excess-decimal rounding is explicitly tested as current storage behavior,
not presented as rejection of fractional-paise input. Deferred balancing,
reconciliation, and C++/concurrency behavior are outside this suite's claims.

The suite checks the database name and requires empty validation tables before
writing. Test helpers isolate every probe in a subtransaction, including
unexpected successes, and match the expected SQLSTATE plus constraint/column
where applicable. Failures are accumulated and reported together. Fixtures
and helper objects are rolled back and final table emptiness is verified.
The psql failure branch raises an error after cleanup; an unexpected setup
error stops execution and may require an explicit ROLLBACK in interactive psql.
Identity sequences may advance; contiguous IDs are not a requirement.

Direct agent TCP access to the user's validation database was retried and
again failed with socket permission error 10013. To check SQL syntax and
behavior without that connection, an independent cluster was initialized in
ignored build/phase2-script-check/pgdata using installed PostgreSQL 18.6,
authentication set to reject, and no listening server started. PostgreSQL
single-user mode executed migrations and the suite's SQL body. The temporary
harness removed psql presentation/branching commands and translated gset's
query terminator; the exact psql wrapper has not yet been run by the agent.
This is isolated-database evidence, not evidence of changes to the user's
running PostgreSQL database or its role permissions.

Measured before fix (migrations 001-003): 58 checks, 54 passed, 4 failed.
All four failures were accepted NaN values on account balance and journal
amount INSERT/UPDATE. NaN is not a valid monetary amount; the existing
balance >= 0 and amount <> 0 checks did not reject it.

Added migration 004_enforce_finite_money.sql with explicit finite-value
constraints on accounts.balance and journal_entries.amount. It validates
existing rows and does not rewrite financial records. Existing migrations and
the C++ implementation are unchanged. The NaN tests now also require the
specific new constraint names.

Measured after applying migration 004 to the isolated cluster: 58 checks,
58 passed, 0 failed; cleanup assertion completed without error. Migration
and suite SQL processes exited zero with no ERROR entries. Before/after raw
outputs are in ignored build/phase2-script-check/phase2_sql_body.log and
phase2_sql_body_after_fix.log; these counts and findings are preserved here.

Next user action: apply migration 004 to ledgercore_validation_20261106, then
run tests/validate_phase2.sql once and provide its summary/failure details.
No migration 004 application or suite pass is yet claimed for that database.
Neither migration 003 nor 004 has been applied by the agent to the original
ledgercore database. No commits or pushes were made.
