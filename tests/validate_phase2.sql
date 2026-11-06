-- Phase 2: database model. Run with psql against the named validation DB.
-- Apply migrations 001 through 004 first. This script does not apply migrations.
-- Fixtures and helper objects are rolled back, including on expected failures.
-- Identity sequences can advance on rollback; consecutive IDs are not required.
-- Deferred journal balancing and C++ transfers belong to phases 3 and 4.
\set ON_ERROR_STOP on
\pset pager off

DO $$
BEGIN
    IF current_database() <> 'ledgercore_validation_20261106' THEN
        RAISE EXCEPTION 'STOP: run only in ledgercore_validation_20261106';
    END IF;
    IF EXISTS (SELECT FROM public.users)
       OR EXISTS (SELECT FROM public.accounts)
       OR EXISTS (SELECT FROM public.transactions)
       OR EXISTS (SELECT FROM public.journal_entries) THEN
        RAISE EXCEPTION 'STOP: validation tables must be empty; no data was deleted';
    END IF;
END;
$$;

BEGIN;
SET LOCAL statement_timeout = '15s';
SET LOCAL lock_timeout = '3s';

CREATE TEMP TABLE phase2_results (
    test_no integer GENERATED ALWAYS AS IDENTITY,
    test_name text NOT NULL,
    passed boolean NOT NULL,
    detail text NOT NULL
) ON COMMIT DROP;

CREATE FUNCTION pg_temp.record_check(label text, ok boolean, explanation text)
RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO pg_temp.phase2_results(test_name, passed, detail)
    VALUES (label, COALESCE(ok, false), explanation);
END;
$$;

-- A deliberately raised private SQLSTATE rolls back even an unexpectedly
-- accepted statement. Never count a different error as the expected rejection.
CREATE FUNCTION pg_temp.expect_error(
    label text, statement text, expected_state text,
    expected_constraint text DEFAULT NULL, expected_column text DEFAULT NULL
) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
    actual_state text;
    actual_constraint text;
    actual_column text;
    actual_message text;
BEGIN
    BEGIN
        EXECUTE statement;
        RAISE SQLSTATE 'ZP001' USING MESSAGE = 'test statement accepted';
    EXCEPTION
        WHEN SQLSTATE 'ZP001' THEN
            actual_state := '00000';
            actual_message := 'Statement was accepted; expected rejection';
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS actual_state = RETURNED_SQLSTATE,
                actual_constraint = CONSTRAINT_NAME,
                actual_column = COLUMN_NAME,
                actual_message = MESSAGE_TEXT;
    END;
    PERFORM pg_temp.record_check(label,
        actual_state = expected_state
        AND (expected_constraint IS NULL OR actual_constraint = expected_constraint)
        AND (expected_column IS NULL OR actual_column = expected_column),
        format('expected SQLSTATE=%s constraint=%s column=%s; got %s: %s',
            expected_state, COALESCE(expected_constraint, '(any)'),
            COALESCE(expected_column, '(any)'), actual_state, actual_message));
END;
$$;

-- Run a single-row RETURNING query, compare its value, and undo its writes.
CREATE FUNCTION pg_temp.expect_value(label text, statement text, expected text)
RETURNS void LANGUAGE plpgsql AS $$
DECLARE
    actual text;
    actual_state text;
    actual_message text;
BEGIN
    BEGIN
        EXECUTE statement INTO STRICT actual;
        RAISE SQLSTATE 'ZP002' USING MESSAGE = 'undo successful value probe';
    EXCEPTION
        WHEN SQLSTATE 'ZP002' THEN NULL;
        WHEN OTHERS THEN
            GET STACKED DIAGNOSTICS actual_state = RETURNED_SQLSTATE,
                actual_message = MESSAGE_TEXT;
    END;
    PERFORM pg_temp.record_check(label,
        actual_state IS NULL AND actual IS NOT DISTINCT FROM expected,
        CASE WHEN actual_state IS NOT NULL
            THEN format('Unexpected %s: %s', actual_state, actual_message)
            ELSE format('expected=%s; actual=%s', expected, actual) END);
END;
$$;

DO $$
DECLARE
    u1 bigint; u2 bigint; u3 bigint;
    a1 bigint; a2 bigint;
    t1 bigint; t2 bigint;
    e1 bigint;
    item record;
    row_id bigint;
    value text;
BEGIN
    -- Valid fixtures are prerequisites. Any unexpected setup error aborts the
    -- suite rather than printing misleading results for dependent checks.
    INSERT INTO public.users(name) VALUES ('P2 suite A') RETURNING id INTO u1;
    INSERT INTO public.users(name) VALUES ('P2 suite B') RETURNING id INTO u2;
    INSERT INTO public.users(name) VALUES ('P2 suite spare') RETURNING id INTO u3;
    INSERT INTO public.accounts(user_id, currency, balance)
        VALUES (u1, 'INR', 100.00) RETURNING id INTO a1;
    INSERT INTO public.accounts(user_id, currency)
        VALUES (u2, 'INR') RETURNING id INTO a2;
    INSERT INTO public.transactions(reference, currency)
        VALUES ('P2-SUITE-A', 'INR') RETURNING id INTO t1;
    INSERT INTO public.transactions(reference, currency)
        VALUES ('P2-SUITE-B', 'INR') RETURNING id INTO t2;
    INSERT INTO public.journal_entries(transaction_id, account_id, amount)
        VALUES (t1, a1, -0.01) RETURNING id INTO e1;
    INSERT INTO public.journal_entries(transaction_id, account_id, amount)
        VALUES (t1, a2, 0.01);

    PERFORM pg_temp.record_check('Valid INR users/accounts/headers/entries accepted',
        (SELECT COUNT(*) = 2 FROM public.journal_entries),
        'Fixture INSERTs succeeded; deferred balancing is not tested here');
    PERFORM pg_temp.record_check('Account default balance is zero',
        (SELECT balance = 0.00 FROM public.accounts WHERE id = a2), 'Expected 0.00');
    PERFORM pg_temp.record_check('Generated IDs are distinct',
        u1 <> u2 AND u1 <> u3 AND u2 <> u3 AND a1 <> a2 AND t1 <> t2,
        'IDs captured from RETURNING, never hard-coded');

    -- Installed currency constraints must be validated CHECK constraints.
    FOR item IN SELECT * FROM (VALUES
        ('accounts', 'chk_account_currency_inr'),
        ('transactions', 'chk_transaction_currency_inr')
    ) AS rules(table_name, constraint_name) LOOP
        PERFORM pg_temp.record_check('Validated constraint: ' || item.constraint_name,
            EXISTS (SELECT FROM pg_constraint
                WHERE conrelid = to_regclass('public.' || item.table_name)
                  AND conname = item.constraint_name AND contype = 'c' AND convalidated),
            'Migration 003 prerequisite');
    END LOOP;

    -- Foreign keys on both creation and modification; parent deletion must fail.
    PERFORM pg_temp.expect_error('Account INSERT: nonexistent user',
        'INSERT INTO public.accounts(user_id,currency) VALUES(-1,''INR'')',
        '23503', 'fk_accounts_user');
    PERFORM pg_temp.expect_error('Account UPDATE: nonexistent user',
        format('UPDATE public.accounts SET user_id=-1 WHERE id=%s', a1),
        '23503', 'fk_accounts_user');
    PERFORM pg_temp.expect_error('Cannot DELETE user with account',
        format('DELETE FROM public.users WHERE id=%s', u1), '23503', 'fk_accounts_user');
    PERFORM pg_temp.expect_error('Journal INSERT: nonexistent transaction',
        format('INSERT INTO public.journal_entries(transaction_id,account_id,amount) VALUES(-1,%s,1)', a1),
        '23503', 'fk_journal_transaction');
    PERFORM pg_temp.expect_error('Journal UPDATE: nonexistent transaction',
        format('UPDATE public.journal_entries SET transaction_id=-1 WHERE id=%s', e1),
        '23503', 'fk_journal_transaction');
    PERFORM pg_temp.expect_error('Journal INSERT: nonexistent account',
        format('INSERT INTO public.journal_entries(transaction_id,account_id,amount) VALUES(%s,-1,1)', t1),
        '23503', 'fk_journal_account');
    PERFORM pg_temp.expect_error('Journal UPDATE: nonexistent account',
        format('UPDATE public.journal_entries SET account_id=-1 WHERE id=%s', e1),
        '23503', 'fk_journal_account');
    PERFORM pg_temp.expect_error('Cannot DELETE account referenced by journal',
        format('DELETE FROM public.accounts WHERE id=%s', a1), '23503', 'fk_journal_account');
    PERFORM pg_temp.expect_error('Cannot DELETE transaction referenced by journal',
        format('DELETE FROM public.transactions WHERE id=%s', t1), '23503', 'fk_journal_transaction');

    PERFORM pg_temp.expect_error('Account INSERT: duplicate user/currency',
        format('INSERT INTO public.accounts(user_id,currency) VALUES(%s,''INR'')', u1),
        '23505', 'uq_account_user_currency');
    PERFORM pg_temp.expect_error('Account UPDATE: duplicate user/currency',
        format('UPDATE public.accounts SET user_id=%s WHERE id=%s', u1, a2),
        '23505', 'uq_account_user_currency');
    PERFORM pg_temp.expect_error('Transaction INSERT: duplicate reference',
        'INSERT INTO public.transactions(reference,currency) VALUES(''P2-SUITE-A'',''INR'')',
        '23505', 'transactions_reference_key');
    PERFORM pg_temp.expect_error('Transaction UPDATE: duplicate reference',
        format('UPDATE public.transactions SET reference=''P2-SUITE-A'' WHERE id=%s', t2),
        '23505', 'transactions_reference_key');

    PERFORM pg_temp.expect_error('Account INSERT: negative balance',
        format('INSERT INTO public.accounts(user_id,currency,balance) VALUES(%s,''INR'',-0.01)', u3),
        '23514', 'chk_account_balance');
    PERFORM pg_temp.expect_error('Account UPDATE: negative balance',
        format('UPDATE public.accounts SET balance=balance-100.01 WHERE id=%s', a1),
        '23514', 'chk_account_balance');
    PERFORM pg_temp.expect_value('Account UPDATE: exactly zero allowed',
        format('UPDATE public.accounts SET balance=balance-100.00 WHERE id=%s RETURNING balance::text', a1), '0.00');
    PERFORM pg_temp.expect_error('Journal INSERT: zero amount',
        format('INSERT INTO public.journal_entries(transaction_id,account_id,amount) VALUES(%s,%s,0)', t1, a1),
        '23514', 'chk_journal_amount_nonzero');
    PERFORM pg_temp.expect_error('Journal UPDATE: zero amount',
        format('UPDATE public.journal_entries SET amount=0 WHERE id=%s', e1),
        '23514', 'chk_journal_amount_nonzero');

    -- Currency enforcement: both tables and both write paths.
    PERFORM pg_temp.expect_error('Account INSERT: USD rejected',
        format('INSERT INTO public.accounts(user_id,currency) VALUES(%s,''USD'')', u3),
        '23514', 'chk_account_currency_inr');
    PERFORM pg_temp.expect_error('Account UPDATE: USD rejected',
        format('UPDATE public.accounts SET currency=''USD'' WHERE id=%s', a1),
        '23514', 'chk_account_currency_inr');
    PERFORM pg_temp.expect_error('Transaction INSERT: USD rejected',
        'INSERT INTO public.transactions(reference,currency) VALUES(''P2-SUITE-USD'',''USD'')',
        '23514', 'chk_transaction_currency_inr');
    PERFORM pg_temp.expect_error('Transaction UPDATE: USD rejected',
        format('UPDATE public.transactions SET currency=''USD'' WHERE id=%s', t1),
        '23514', 'chk_transaction_currency_inr');
    PERFORM pg_temp.expect_value('Account UPDATE: INR accepted',
        format('UPDATE public.accounts SET currency=''INR'' WHERE id=%s RETURNING currency::text', a1), 'INR');
    PERFORM pg_temp.expect_value('Transaction UPDATE: INR accepted',
        format('UPDATE public.transactions SET currency=''INR'' WHERE id=%s RETURNING currency::text', t1), 'INR');

    -- All explicitly required, non-identity columns must reject NULL on UPDATE.
    -- Using existing valid rows avoids unrelated FK/uniqueness failures.
    FOR item IN SELECT * FROM (VALUES
        ('users','name'), ('users','created_at'),
        ('accounts','user_id'), ('accounts','currency'),
        ('accounts','balance'), ('accounts','created_at'),
        ('transactions','reference'), ('transactions','currency'), ('transactions','created_at'),
        ('journal_entries','transaction_id'), ('journal_entries','account_id'),
        ('journal_entries','amount'), ('journal_entries','created_at')
    ) AS required(table_name, column_name) LOOP
        row_id := CASE item.table_name WHEN 'users' THEN u1 WHEN 'accounts' THEN a1
            WHEN 'transactions' THEN t1 ELSE e1 END;
        PERFORM pg_temp.expect_error('NULL rejected: ' || item.table_name || '.' || item.column_name,
            format('UPDATE public.%I SET %I=NULL WHERE id=%s', item.table_name, item.column_name, row_id),
            '23502', NULL, item.column_name);
    END LOOP;
    PERFORM pg_temp.expect_error('Journal INSERT: NULL amount',
        format('INSERT INTO public.journal_entries(transaction_id,account_id,amount) VALUES(%s,%s,NULL)', t1, a1),
        '23502', NULL, 'amount');

    -- Exact cent storage, numeric range, and the known scale-rounding behavior.
    PERFORM pg_temp.expect_value('Money: exact 0.10 + 0.20 storage',
        format('UPDATE public.accounts SET balance=0.10::numeric+0.20::numeric WHERE id=%s RETURNING balance::text', a1), '0.30');
    PERFORM pg_temp.expect_value('Money: NUMERIC(20,2) upper finite boundary',
        format('UPDATE public.accounts SET balance=999999999999999999.99 WHERE id=%s RETURNING balance::text', a1),
        '999999999999999999.99');
    PERFORM pg_temp.expect_error('Money: account overflow rejected',
        format('UPDATE public.accounts SET balance=1000000000000000000 WHERE id=%s', a1), '22003');
    PERFORM pg_temp.expect_error('Money: journal overflow rejected',
        format('UPDATE public.journal_entries SET amount=1000000000000000000 WHERE id=%s', e1), '22003');
    PERFORM pg_temp.expect_value('Storage behavior: excess decimals round (not reject)',
        format('UPDATE public.accounts SET balance=1.005 WHERE id=%s RETURNING balance::text', a1), '1.01');

    -- Money must be a finite number. Report accepted NaN as a real gap rather
    -- than assuming >= 0 and <> 0 constraints reject every invalid numeric.
    PERFORM pg_temp.expect_error('Account INSERT: NaN must be rejected',
        format('INSERT INTO public.accounts(user_id,currency,balance) VALUES(%s,''INR'',''NaN''::numeric)', u3),
        '23514', 'chk_account_balance_finite');
    PERFORM pg_temp.expect_error('Account UPDATE: NaN must be rejected',
        format('UPDATE public.accounts SET balance=''NaN''::numeric WHERE id=%s', a1),
        '23514', 'chk_account_balance_finite');
    PERFORM pg_temp.expect_error('Journal INSERT: NaN must be rejected',
        format('INSERT INTO public.journal_entries(transaction_id,account_id,amount) VALUES(%s,%s,''NaN''::numeric)', t1, a1),
        '23514', 'chk_journal_amount_finite');
    PERFORM pg_temp.expect_error('Journal UPDATE: NaN must be rejected',
        format('UPDATE public.journal_entries SET amount=''NaN''::numeric WHERE id=%s', e1),
        '23514', 'chk_journal_amount_finite');
    FOREACH value IN ARRAY ARRAY['Infinity', '-Infinity'] LOOP
        PERFORM pg_temp.expect_error('Account: ' || value || ' rejected by numeric type',
            format('UPDATE public.accounts SET balance=%L::numeric WHERE id=%s', value, a1), '22003');
        PERFORM pg_temp.expect_error('Journal: ' || value || ' rejected by numeric type',
            format('UPDATE public.journal_entries SET amount=%L::numeric WHERE id=%s', value, e1), '22003');
    END LOOP;

    PERFORM pg_temp.record_check('Failed probes preserve original account balance/currency',
        (SELECT balance=100.00 AND currency='INR' FROM public.accounts WHERE id=a1),
        'All probe subtransactions were rolled back');
    PERFORM pg_temp.record_check('Failed probes preserve original journal amounts',
        (SELECT COUNT(*)=2 AND SUM(amount)=0.00 FROM public.journal_entries WHERE transaction_id=t1),
        'Two fixture entries remain; no commit-time trigger claim');
END;
$$;

SELECT test_no, CASE WHEN passed THEN 'PASS' ELSE 'FAIL' END AS result, test_name
FROM pg_temp.phase2_results ORDER BY test_no;

SELECT test_name, detail FROM pg_temp.phase2_results WHERE NOT passed ORDER BY test_no;

SELECT COUNT(*) AS total_checks,
       COUNT(*) FILTER (WHERE passed) AS passed,
       COUNT(*) FILTER (WHERE NOT passed) AS failed
FROM pg_temp.phase2_results;

SELECT EXISTS (SELECT FROM pg_temp.phase2_results WHERE NOT passed) AS phase2_failed
\gset

ROLLBACK;

DO $$
BEGIN
    IF EXISTS (SELECT FROM public.users) OR EXISTS (SELECT FROM public.accounts)
       OR EXISTS (SELECT FROM public.transactions) OR EXISTS (SELECT FROM public.journal_entries) THEN
        RAISE EXCEPTION 'FAIL: cleanup did not restore empty validation tables';
    END IF;
END;
$$;
\echo 'Cleanup verified: all four tables are empty. Identity sequence gaps are normal.'

\if :phase2_failed
    DO $$ BEGIN RAISE EXCEPTION 'PHASE 2 FAILED: see failed checks above. Fixtures were rolled back.'; END; $$;
\else
    \echo 'PHASE 2 PASS: database-model checks passed. Phases 3 and 4 are not certified by this script.'
\endif
