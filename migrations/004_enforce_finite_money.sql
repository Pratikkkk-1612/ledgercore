-- LedgerCore migration 004: money must be a finite decimal number.
-- NUMERIC accepts NaN, and the existing >= 0 / <> 0 checks do not reject it.
-- Existing rows are validated. Invalid records are never silently rewritten.
-- Apply once per database; both constraints are installed atomically.

BEGIN;

ALTER TABLE public.accounts
    ADD CONSTRAINT chk_account_balance_finite
    CHECK (balance NOT IN ('NaN'::numeric, 'Infinity'::numeric, '-Infinity'::numeric));

ALTER TABLE public.journal_entries
    ADD CONSTRAINT chk_journal_amount_finite
    CHECK (amount NOT IN ('NaN'::numeric, 'Infinity'::numeric, '-Infinity'::numeric));

COMMIT;
