-- LedgerCore migration 003: enforce the current INR-only scope.
-- Existing rows are validated; non-INR data makes this migration fail.
-- This does not convert currencies or rewrite financial records.
-- Apply once per database. Both constraints are installed atomically.

BEGIN;

ALTER TABLE public.accounts
    ADD CONSTRAINT chk_account_currency_inr
    CHECK (currency = 'INR');

ALTER TABLE public.transactions
    ADD CONSTRAINT chk_transaction_currency_inr
    CHECK (currency = 'INR');

COMMIT;
