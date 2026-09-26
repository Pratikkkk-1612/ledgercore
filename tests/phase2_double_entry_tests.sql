-- ============================================================
-- TEST 1: Valid transaction must COMMIT
-- ============================================================

BEGIN;

INSERT INTO transactions (reference, currency)
VALUES ('PHASE2-VALID-001', 'INR')
RETURNING id \gset valid_tx_

INSERT INTO journal_entries (transaction_id, account_id, amount)
VALUES
    (:valid_tx_id, 1, -200.00),
    (:valid_tx_id, 2,  200.00);

COMMIT;

SELECT
    t.reference,
    t.currency,
    SUM(e.amount) AS journal_sum
FROM transactions t
JOIN journal_entries e ON e.transaction_id = t.id
WHERE t.reference = 'PHASE2-VALID-001'
GROUP BY t.id, t.reference, t.currency;


-- ============================================================
-- TEST 2: Invalid transaction must be rejected
-- ============================================================

BEGIN;

INSERT INTO transactions (reference, currency)
VALUES ('PHASE2-INVALID-001', 'INR')
RETURNING id \gset invalid_tx_

INSERT INTO journal_entries (transaction_id, account_id, amount)
VALUES
    (:invalid_tx_id, 1, -100.00),
    (:invalid_tx_id, 2,   90.00);

COMMIT;

SELECT id, reference
FROM transactions
WHERE reference = 'PHASE2-INVALID-001';