#include <iostream>
#include <string>
#include <cstdlib>
#include <libpq-fe.h>

// ------------------------------------------------------------
// Execute a SQL command that does not return rows
// ------------------------------------------------------------

bool executeQuery(PGconn* conn, const char* query)
{
    PGresult* result = PQexec(conn, query);
 
    if (PQresultStatus(result) != PGRES_COMMAND_OK)
    {
        std::cerr << "Query failed:\n"
                  << PQerrorMessage(conn)
                  << std::endl;

        PQclear(result);
        return false;
    }

    PQclear(result);
    return true;
}

// ------------------------------------------------------------
// Execute a parameterized SQL command
// ------------------------------------------------------------

bool executeParameterizedCommand(
    PGconn* conn,
    const char* query,
    int parameterCount,
    const char* const* parameterValues,
    int* rowsAffected = nullptr)
{
    PGresult* result = PQexecParams(
        conn,
        query,
        parameterCount,
        nullptr,
        parameterValues,
        nullptr,
        nullptr,
        0
    );

    if (PQresultStatus(result) != PGRES_COMMAND_OK)
    {
        std::cerr << "Parameterized query failed:\n"
                  << PQerrorMessage(conn)
                  << std::endl;

        PQclear(result);
        return false;
    }

    if (rowsAffected != nullptr)
    {
        *rowsAffected = std::atoi(PQcmdTuples(result));
    }

    PQclear(result);
    return true;
}

// ------------------------------------------------------------
// Execute a parameterized query that returns rows
// ------------------------------------------------------------

PGresult* executeParameterizedQuery(
    PGconn* conn,
    const char* query,
    int parameterCount,
    const char* const* parameterValues)
{
    PGresult* result = PQexecParams(
        conn,
        query,
        parameterCount,
        nullptr,
        parameterValues,
        nullptr,
        nullptr,
        0
    );

    if (PQresultStatus(result) != PGRES_TUPLES_OK)
    {
        std::cerr << "Parameterized query failed:\n"
                  << PQerrorMessage(conn)
                  << std::endl;

        PQclear(result);
        return nullptr;
    }

    return result;
}

// ------------------------------------------------------------
// Convert paise to an exact decimal SQL value.
//
// Example:
// 10000 paise -> "100.00"
// 150 paise  -> "1.50"
// ------------------------------------------------------------

std::string formatAmount(long long amountPaise)
{
    long long rupees = amountPaise / 100;
    long long paise = amountPaise % 100;

    std::string result =
        std::to_string(rupees) + ".";

    if (paise < 10)
    {
        result += "0";
    }

    result += std::to_string(paise);

    return result;
}

// ------------------------------------------------------------
// Perform an atomic money transfer
//
// amountPaise:
//     Money is represented as an integer number of paise.
//     Example: ₹100.00 = 10000 paise.
//
// injectFailure:
//     Test-only switch used to verify rollback.
// ------------------------------------------------------------

bool transfer(
    PGconn* conn,
    int sourceAccountId,
    int destinationAccountId,
    long long amountPaise,
    const std::string& reference,
    bool injectFailure = false)
{
    // --------------------------------------------------------
    // Basic validation
    // --------------------------------------------------------

    if (sourceAccountId == destinationAccountId)
    {
        std::cerr << "Transfer failed: source and destination "
                  << "accounts must be different."
                  << std::endl;

        return false;
    }

    if (amountPaise <= 0)
    {
        std::cerr << "Transfer failed: amount must be positive."
                  << std::endl;

        return false;
    }

    std::string amount =
        formatAmount(amountPaise);

    std::string negativeAmount =
        "-" + amount;

    std::cout << "\nStarting transfer: "
              << sourceAccountId
              << " -> "
              << destinationAccountId
              << " | Amount: "
              << amount
              << " INR"
              << std::endl;

    // --------------------------------------------------------
    // 1. Start transaction
    // --------------------------------------------------------

    if (!executeQuery(conn, "BEGIN;"))
    {
        return false;
    }

    // --------------------------------------------------------
    // 2. Debit source account
    // --------------------------------------------------------

    std::string sourceAccount =
        std::to_string(sourceAccountId);

    const char* debitParameters[] =
    {
        sourceAccount.c_str(),
        amount.c_str()
    };

    int rowsAffected = 0;

    if (!executeParameterizedCommand(
            conn,
            "UPDATE accounts "
            "SET balance = balance - $2 "
            "WHERE id = $1 "
            "AND balance >= $2;",
            2,
            debitParameters,
            &rowsAffected))
    {
        executeQuery(conn, "ROLLBACK;");
        return false;
    }

    if (rowsAffected != 1)
    {
        std::cerr << "Debit failed: account does not exist "
                  << "or insufficient funds."
                  << std::endl;

        executeQuery(conn, "ROLLBACK;");
        return false;
    }

    std::cout << "Source account debited."
              << std::endl;

    // --------------------------------------------------------
    // 3. Credit destination account
    // --------------------------------------------------------

    std::string destinationAccount =
        std::to_string(destinationAccountId);

    const char* creditParameters[] =
    {
        destinationAccount.c_str(),
        amount.c_str()
    };

    rowsAffected = 0;

    if (!executeParameterizedCommand(
            conn,
            "UPDATE accounts "
            "SET balance = balance + $2 "
            "WHERE id = $1;",
            2,
            creditParameters,
            &rowsAffected))
    {
        executeQuery(conn, "ROLLBACK;");
        return false;
    }

    if (rowsAffected != 1)
    {
        std::cerr << "Credit failed: destination account "
                  << "does not exist."
                  << std::endl;

        executeQuery(conn, "ROLLBACK;");
        return false;
    }

    std::cout << "Destination account credited."
              << std::endl;

    // --------------------------------------------------------
    // 4. Create transaction record
    //
    // RETURNING id gives us the exact transaction ID without
    // querying the transaction table again by reference.
    // --------------------------------------------------------

    const char* transactionParameters[] =
    {
        reference.c_str(),
        "INR"
    };

    PGresult* transactionResult =
        executeParameterizedQuery(
            conn,
            "INSERT INTO transactions(reference, currency) "
            "VALUES ($1, $2) "
            "RETURNING id;",
            2,
            transactionParameters
        );

    if (transactionResult == nullptr)
    {
        std::cerr << "Transaction record creation failed."
                  << std::endl;

        executeQuery(conn, "ROLLBACK;");
        return false;
    }

    if (PQntuples(transactionResult) != 1)
    {
        std::cerr << "Transaction creation returned "
                  << "an unexpected result."
                  << std::endl;

        PQclear(transactionResult);
        executeQuery(conn, "ROLLBACK;");
        return false;
    }

    std::string transactionId =
        PQgetvalue(transactionResult, 0, 0);

    PQclear(transactionResult);

    std::cout << "Transaction record created. ID: "
              << transactionId
              << std::endl;

    // --------------------------------------------------------
    // 5. Create journal entries
    // --------------------------------------------------------

    const char* journalParameters[] =
    {
        transactionId.c_str(),
        sourceAccount.c_str(),
        negativeAmount.c_str(),
        destinationAccount.c_str(),
        amount.c_str()
    };

    rowsAffected = 0;

    if (!executeParameterizedCommand(
            conn,
            "INSERT INTO journal_entries("
            "transaction_id, account_id, amount"
            ") "
            "VALUES "
            "($1, $2, $3), "
            "($1, $4, $5);",
            5,
            journalParameters,
            &rowsAffected))
    {
        std::cerr << "Journal entry creation failed."
                  << std::endl;

        executeQuery(conn, "ROLLBACK;");
        return false;
    }

    if (rowsAffected != 2)
    {
        std::cerr << "Journal entry creation failed: expected "
                  << "2 rows, but created "
                  << rowsAffected
                  << " rows."
                  << std::endl;

        executeQuery(conn, "ROLLBACK;");
        return false;
    }

    std::cout << "Journal entries created."
              << std::endl;

    // --------------------------------------------------------
    // 6. Controlled failure injection
    //
    // This is used only during Phase 3 rollback validation.
    // The invalid user ID intentionally violates the FK.
    // --------------------------------------------------------

    if (injectFailure)
    {
        std::cout << "\nInjecting deliberate failure..."
                  << std::endl;

        const char* failureParameters[] =
        {
            "999999",
            "INR",
            "0.00"
        };

        bool failureQuerySucceeded =
            executeParameterizedCommand(
                conn,
                "INSERT INTO accounts(user_id, currency, balance) "
                "VALUES ($1, $2, $3);",
                3,
                failureParameters
            );

        if (failureQuerySucceeded)
        {
            std::cerr << "ERROR: deliberate failure "
                      << "did not occur."
                      << std::endl;

            executeQuery(conn, "ROLLBACK;");
            return false;
        }

        std::cout << "Expected failure occurred."
                  << std::endl;

        std::cout << "Rolling back transaction..."
                  << std::endl;

        if (!executeQuery(conn, "ROLLBACK;"))
        {
            std::cerr << "ROLLBACK failed."
                      << std::endl;

            return false;
        }

        std::cout << "Transaction rolled back successfully."
                  << std::endl;

        return false;
    }

    // --------------------------------------------------------
    // 7. Commit transaction
    // --------------------------------------------------------

    if (!executeQuery(conn, "COMMIT;"))
    {
        std::cerr << "COMMIT failed."
                  << std::endl;

        executeQuery(conn, "ROLLBACK;");
        return false;
    }

    std::cout << "Transfer committed successfully."
              << std::endl;

    return true;
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------

int main()
{
    std::cout << "LedgerCore starting..."
              << std::endl;

    // --------------------------------------------------------
    // Connect to PostgreSQL
    // --------------------------------------------------------

    PGconn* conn = PQconnectdb(
        "host=127.0.0.1 "
        "port=5432 "
        "dbname=ledgercore "
        "user=ledgercore"
    );

    if (PQstatus(conn) != CONNECTION_OK)
    {
        std::cerr << "Database connection failed:\n"
                  << PQerrorMessage(conn)
                  << std::endl;

        PQfinish(conn);
        return 1;
    }

    std::cout << "Connected to PostgreSQL successfully."
              << std::endl;

    // --------------------------------------------------------
    // Check whether rollback test mode is enabled
    // --------------------------------------------------------

    const char* rollbackTest =
        std::getenv("LEDGERCORE_TEST_ROLLBACK");

    bool runRollbackTest =
        rollbackTest != nullptr &&
        std::string(rollbackTest) == "1";

    // --------------------------------------------------------
    // Execute transfer
    // --------------------------------------------------------

    bool success;

    if (runRollbackTest)
    {
        success = transfer(
            conn,
            1,
            2,
            10000,  // ₹100.00
            "PHASE3-FINAL-ROLLBACK-001",
            true
        );
    }
    else
    {
        success = transfer(
            conn,
            1,
            2,
            10000,  // ₹100.00
            "PHASE3-FINAL-001"
        );
    }

    // --------------------------------------------------------
    // Print result
    // --------------------------------------------------------

    if (runRollbackTest)
    {
        if (!success)
        {
            std::cout << "\nPhase 3 rollback test completed."
                      << std::endl;
        }
        else
        {
            std::cerr << "\nERROR: rollback test unexpectedly "
                      << "succeeded."
                      << std::endl;
        }
    }
    else
    {
        if (success)
        {
            std::cout << "\nTransfer completed successfully."
                      << std::endl;
        }
        else
        {
            std::cout << "\nTransfer failed."
                      << std::endl;
        }
    }

    // --------------------------------------------------------
    // Close connection
    // --------------------------------------------------------

    PQfinish(conn);

    // An expected rollback-test failure is a successful test.
    if (runRollbackTest && !success)
    {
        return 0;
    }

    return success ? 0 : 1;
}