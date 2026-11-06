
#include <iostream>
#include <string>
#include <cstdlib>
#include <libpq-fe.h>

// ------------------------------------------------------------
// Execute a SQL command without parameters
// ------------------------------------------------------------

bool executeQuery(PGconn* conn, const char* query)
{
    PGresult* result = PQexec(conn, query);

    if (result == nullptr)
    {
        std::cerr << "Query execution failed:\n"
                  << PQerrorMessage(conn)
                  << std::endl;
        return false;
    }

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
// Optionally returns affected rows and SQLSTATE.
// ------------------------------------------------------------

bool executeParameterizedCommand(
    PGconn* conn,
    const char* query,
    int parameterCount,
    const char* const* parameterValues,
    int* rowsAffected = nullptr,
    std::string* sqlState = nullptr)
{
    if (sqlState != nullptr)
    {
        sqlState->clear();
    }

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

    if (result == nullptr)
    {
        std::cerr << "Parameterized query execution failed:\n"
                  << PQerrorMessage(conn)
                  << std::endl;
        return false;
    }

    if (PQresultStatus(result) != PGRES_COMMAND_OK)
    {
        if (sqlState != nullptr)
        {
            const char* state =
                PQresultErrorField(result, PG_DIAG_SQLSTATE);

            if (state != nullptr)
            {
                *sqlState = state;
            }
        }

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
// Used for INSERT ... RETURNING.
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

    if (result == nullptr)
    {
        std::cerr << "Parameterized query execution failed:\n"
                  << PQerrorMessage(conn)
                  << std::endl;
        return nullptr;
    }

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
// Convert paise into an exact decimal SQL value.
//
// 10000 paise = 100.00
// 150 paise   = 1.50
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
//     Money represented as integer paise.
//
// injectFailure:
//     Enables controlled rollback testing.
//
// expectedFailureObserved:
//     True only when the expected FK error occurs
//     and ROLLBACK succeeds.
// ------------------------------------------------------------

bool transfer(
    PGconn* conn,
    long long sourceAccountId,
    long long destinationAccountId,
    long long amountPaise,
    const std::string& reference,
    bool injectFailure = false,
    bool* expectedFailureObserved = nullptr)
{
    if (expectedFailureObserved != nullptr)
    {
        *expectedFailureObserved = false;
    }

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

    const std::string amount =
        formatAmount(amountPaise);

    const std::string negativeAmount =
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

    const std::string sourceAccount =
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

    const std::string destinationAccount =
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
        std::cerr << "Unexpected transaction result."
                  << std::endl;

        PQclear(transactionResult);
        executeQuery(conn, "ROLLBACK;");
        return false;
    }

    const std::string transactionId =
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
                  << "."
                  << std::endl;

        executeQuery(conn, "ROLLBACK;");
        return false;
    }

    std::cout << "Journal entries created."
              << std::endl;

    // --------------------------------------------------------
    // 6. Controlled failure injection
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

        std::string actualSqlState;

        bool failureQuerySucceeded =
            executeParameterizedCommand(
                conn,
                "INSERT INTO accounts(user_id, currency, balance) "
                "VALUES ($1, $2, $3);",
                3,
                failureParameters,
                nullptr,
                &actualSqlState
            );

        // Failure must actually occur.

        if (failureQuerySucceeded)
        {
            std::cerr << "ERROR: Deliberate failure did not occur."
                      << std::endl;

            executeQuery(conn, "ROLLBACK;");
            return false;
        }

        // Only SQLSTATE 23503 is expected.

        if (actualSqlState != "23503")
        {
            std::cerr << "Unexpected database error. SQLSTATE: "
                      << actualSqlState
                      << std::endl;

            executeQuery(conn, "ROLLBACK;");
            return false;
        }

        std::cout << "Expected foreign-key violation confirmed."
                  << std::endl;

        // Rollback must succeed.

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

        // Mark test as passed only after expected error
        // and successful rollback.

        if (expectedFailureObserved != nullptr)
        {
            *expectedFailureObserved = true;
        }

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
    // 1. Connect to validation database
    // --------------------------------------------------------

    PGconn* conn = PQconnectdb(
        "host=127.0.0.1 "
        "port=5432 "
        "dbname=ledgercore_validation_20261106 "
        "user=ledgercore"
    );

    if (conn == nullptr || PQstatus(conn) != CONNECTION_OK)
    {
        std::cerr << "Database connection failed:\n";

        if (conn != nullptr)
        {
            std::cerr << PQerrorMessage(conn);
            PQfinish(conn);
        }

        return 1;
    }

    std::cout << "Connected to PostgreSQL successfully."
              << std::endl;

    // --------------------------------------------------------
    // 2. Determine test mode
    // --------------------------------------------------------

    const char* rollbackTest =
        std::getenv("LEDGERCORE_TEST_ROLLBACK");

    const bool runRollbackTest =
        rollbackTest != nullptr &&
        std::string(rollbackTest) == "1";

    bool success = false;
    bool expectedFailureObserved = false;

    // --------------------------------------------------------
    // 3. Execute transfer
    // --------------------------------------------------------

    if (runRollbackTest)
    {
        std::cout << "\nRunning rollback validation..."
                  << std::endl;

        success = transfer(
            conn,
            19,
            20,
            10000,
            "P4-ROLLBACK-002",
            true,
            &expectedFailureObserved
        );
    }
    else
    {
        std::cout << "\nRunning normal transfer..."
                  << std::endl;

        success = transfer(
            conn,
            19,
            20,
            10000,
            "P4-SUCCESS-002",
            false
        );
    }

    // --------------------------------------------------------
    // 4. Evaluate result
    // --------------------------------------------------------

    bool testPassed = false;

    if (runRollbackTest)
    {
        if (!success && expectedFailureObserved)
        {
            std::cout << "\nROLLBACK TEST PASSED."
                      << std::endl;

            testPassed = true;
        }
        else
        {
            std::cerr << "\nROLLBACK TEST FAILED."
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
            std::cerr << "\nTransfer failed."
                      << std::endl;
        }
    }

    // --------------------------------------------------------
    // 5. Close connection
    // --------------------------------------------------------

    PQfinish(conn);

    if (runRollbackTest)
    {
        return testPassed ? 0 : 1;
    }

    return success ? 0 : 1;
}
