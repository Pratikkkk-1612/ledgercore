-- LedgerCore migration 002
-- Enforce the transaction-level double-entry invariant:
--     SUM(journal_entries.amount) = 0 for every transaction.
--
-- This is intentionally a PostgreSQL constraint trigger and is initially
-- deferred so a transaction can insert its debit and credit legs before
-- the invariant is checked.

BEGIN;

CREATE OR REPLACE FUNCTION enforce_balanced_transaction()
RETURNS trigger
LANGUAGE plpgsql
AS $$
DECLARE
    tx_id bigint;
    tx_total numeric(20,2);
BEGIN
    -- Determine which transaction(s) need to be revalidated.
    IF TG_OP = 'DELETE' THEN
        tx_id := OLD.transaction_id;
    ELSE
        tx_id := NEW.transaction_id;
    END IF;

    -- Serialize changes to the parent transaction row. This prevents two
    -- concurrent writers from independently passing the aggregate check.
    PERFORM 1
    FROM transactions
    WHERE id = tx_id
    FOR UPDATE;

    SELECT COALESCE(SUM(amount), 0)
      INTO tx_total
      FROM journal_entries
     WHERE transaction_id = tx_id;

    IF tx_total <> 0 THEN
        RAISE EXCEPTION
            'Transaction % is unbalanced: journal entry sum = %',
            tx_id,
            tx_total
            USING ERRCODE = '23514';
    END IF;

    -- If an entry is moved from one transaction to another, validate the
    -- old transaction as well. In the intended application flow,
    -- transaction_id should normally be immutable.
    IF TG_OP = 'UPDATE'
       AND OLD.transaction_id IS DISTINCT FROM NEW.transaction_id THEN

        PERFORM 1
        FROM transactions
        WHERE id = OLD.transaction_id
        FOR UPDATE;

        SELECT COALESCE(SUM(amount), 0)
          INTO tx_total
          FROM journal_entries
         WHERE transaction_id = OLD.transaction_id;

        IF tx_total <> 0 THEN
            RAISE EXCEPTION
                'Transaction % is unbalanced after journal entry move: sum = %',
                OLD.transaction_id,
                tx_total
                USING ERRCODE = '23514';
        END IF;
    END IF;

    RETURN NULL;
END;
$$;

DROP TRIGGER IF EXISTS trg_journal_entries_balanced
ON journal_entries;

CREATE CONSTRAINT TRIGGER trg_journal_entries_balanced
AFTER INSERT OR UPDATE OR DELETE
ON journal_entries
DEFERRABLE INITIALLY DEFERRED
FOR EACH ROW
EXECUTE FUNCTION enforce_balanced_transaction();

COMMIT;
