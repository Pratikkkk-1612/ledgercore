#include "../src/transfer.hpp"
#include "concurrency_hooks.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;

namespace {
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    int arrived = 0;
    int target = 0; // zero: controller releases; two: both debits must finish
    bool released = false;
    bool timedOut = false;

    bool arrive() {
        std::unique_lock lock(mutex);
        ++arrived;
        if (target > 0 && arrived >= target) released = true;
        cv.notify_all();
        if (!cv.wait_for(lock, 10s, [&] { return released; })) {
            timedOut = true;
            released = true;
            cv.notify_all();
        }
        return !timedOut;
    }
    bool awaitFirst() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 5s, [&] { return arrived > 0; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};

thread_local Gate* workerGate = nullptr;
thread_local std::string firstSqlState;

Result query(PGconn* conn, const std::string& sql,
             const std::vector<std::string>& args = {}) {
    std::vector<const char*> values;
    for (const auto& arg : args) values.push_back(arg.c_str());
    Result result(PQexecParams(conn, sql.c_str(), static_cast<int>(values.size()),
                              nullptr, values.data(), nullptr, nullptr, 0), PQclear);
    if (!result || (PQresultStatus(result.get()) != PGRES_COMMAND_OK &&
                    PQresultStatus(result.get()) != PGRES_TUPLES_OK)) {
        throw std::runtime_error(result ? PQresultErrorMessage(result.get()) : PQerrorMessage(conn));
    }
    return result;
}

std::string scalar(PGconn* conn, const std::string& sql,
                   const std::vector<std::string>& args = {}) {
    auto result = query(conn, sql, args);
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 1 ||
        PQgetisnull(result.get(), 0, 0)) throw std::runtime_error("Expected one non-NULL scalar");
    return PQgetvalue(result.get(), 0, 0);
}

Connection connect() {
    const char* supplied = std::getenv("LEDGERCORE_TEST_CONNINFO");
    const std::string info = supplied ? supplied :
        "host=127.0.0.1 port=5432 dbname=ledgercore_validation_20261106 user=ledgercore connect_timeout=5";
    Connection conn(PQconnectdb(info.c_str()), PQfinish);
    if (!conn || PQstatus(conn.get()) != CONNECTION_OK)
        throw std::runtime_error(conn ? PQerrorMessage(conn.get()) : "Could not allocate connection");
    if (std::string(PQdb(conn.get())) != "ledgercore_validation_20261106")
        throw std::runtime_error("Refusing to run outside ledgercore_validation_20261106");
    query(conn.get(), "SET default_transaction_isolation = 'read committed'");
    query(conn.get(), "SET statement_timeout = '20s'");
    query(conn.get(), "SET lock_timeout = '15s'");
    query(conn.get(), "SET search_path = public");
    query(conn.get(), "SET application_name = 'ledgercore_phase5'");
    return conn;
}

void rollback(PGconn* conn) {
    Result result(PQexec(conn, "ROLLBACK"), PQclear);
}

// Only IDs created by this runner and its two unique references are cleaned up.
// Other validation data (including accounts 19/20) is never reset or deleted.
struct Fixtures {
    PGconn* conn;
    std::vector<long long> users;
    std::vector<long long> accounts;
    std::vector<std::string> refs;
    bool cleaned = false;

    Fixtures(PGconn* c, const std::string& tag, const std::vector<std::string>& balances) : conn(c) {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const std::string prefix = "P5-" + tag + "-" + std::to_string(stamp);
        refs = {prefix + "-1", prefix + "-2"};
        if (scalar(conn, "SELECT count(*) FROM transactions WHERE reference IN ($1,$2)", refs) != "0")
            throw std::runtime_error("Fixture references already exist");
        query(conn, "BEGIN");
        try {
            for (size_t i = 0; i < balances.size(); ++i) {
                const auto uid = std::stoll(scalar(conn,
                    "INSERT INTO users(name) VALUES($1) RETURNING id", {prefix + "-user-" + std::to_string(i)}));
                users.push_back(uid);
                accounts.push_back(std::stoll(scalar(conn,
                    "INSERT INTO accounts(user_id,currency,balance) VALUES($1,'INR',$2) RETURNING id",
                    {std::to_string(uid), balances[i]})));
            }
            query(conn, "COMMIT");
        } catch (...) { rollback(conn); throw; }
        std::cout << "Fixture references: " << refs[0] << ", " << refs[1] << '\n';
    }

    long long balance(size_t i) {
        return std::stoll(scalar(conn, "SELECT (balance*100)::bigint FROM accounts WHERE id=$1",
                                 {std::to_string(accounts.at(i))}));
    }

    bool ledger(size_t worker, bool committed, size_t source, size_t destination, const std::string& amount) {
        const auto count = scalar(conn, "SELECT count(*) FROM transactions WHERE reference=$1", {refs[worker]});
        if (count != (committed ? "1" : "0")) return false;
        if (!committed) return true; // FK means entries cannot survive without the header.
        return scalar(conn,
            "SELECT count(*)=2 AND sum(j.amount)=0 "
            "AND count(*) FILTER(WHERE j.account_id=$2 AND j.amount=-$4::numeric)=1 "
            "AND count(*) FILTER(WHERE j.account_id=$3 AND j.amount=$4::numeric)=1 "
            "FROM journal_entries j JOIN transactions t ON t.id=j.transaction_id WHERE t.reference=$1",
            {refs[worker], std::to_string(accounts[source]), std::to_string(accounts[destination]), amount}) == "t";
    }

    void cleanup() {
        if (cleaned) return;
        query(conn, "BEGIN");
        try {
            query(conn, "DELETE FROM journal_entries WHERE transaction_id IN "
                        "(SELECT id FROM transactions WHERE reference IN ($1,$2))", refs);
            query(conn, "DELETE FROM transactions WHERE reference IN ($1,$2)", refs);
            for (const auto id : accounts) query(conn, "DELETE FROM accounts WHERE id=$1", {std::to_string(id)});
            for (const auto id : users) query(conn, "DELETE FROM users WHERE id=$1", {std::to_string(id)});
            query(conn, "COMMIT");
        } catch (...) { rollback(conn); throw; }
        if (scalar(conn, "SELECT count(*) FROM transactions WHERE reference IN ($1,$2)", refs) != "0")
            throw std::runtime_error("Fixture transaction cleanup failed");
        for (const auto id : accounts)
            if (scalar(conn, "SELECT count(*) FROM accounts WHERE id=$1", {std::to_string(id)}) != "0")
                throw std::runtime_error("Fixture account cleanup failed");
        for (const auto id : users)
            if (scalar(conn, "SELECT count(*) FROM users WHERE id=$1", {std::to_string(id)}) != "0")
                throw std::runtime_error("Fixture user cleanup failed");
        cleaned = true;
        std::cout << "Fixture cleanup verified.\n";
    }

    ~Fixtures() {
        if (!cleaned) {
            try { cleanup(); }
            catch (const std::exception& e) {
                std::cerr << "CLEANUP ERROR: " << e.what() << "\nInspect fixture references above.\n";
            }
        }
    }
};

struct Outcome {
    bool success = false;
    bool idle = false;
    std::string sqlstate;
    std::string exception;
};

void runWorker(PGconn* conn, long long from, long long to, long long paise,
               const std::string& reference, Gate* gate, Outcome& outcome) {
    workerGate = gate;
    firstSqlState.clear();
    try { outcome.success = transfer(conn, from, to, paise, reference); }
    catch (const std::exception& e) { outcome.exception = e.what(); rollback(conn); }
    catch (...) { outcome.exception = "Unknown worker exception"; rollback(conn); }
    outcome.sqlstate = firstSqlState;
    outcome.idle = PQstatus(conn) == CONNECTION_OK && PQtransactionStatus(conn) == PQTRANS_IDLE;
    workerGate = nullptr;
}

void show(const char* name, const Outcome& outcome) {
    std::cout << name << ": committed=" << outcome.success
              << ", SQLSTATE=" << (outcome.sqlstate.empty() ? "none" : outcome.sqlstate)
              << ", connection idle=" << outcome.idle;
    if (!outcome.exception.empty()) std::cout << ", exception=" << outcome.exception;
    std::cout << '\n';
}

bool competing(PGconn* observer) {
    std::cout << "\nEXPERIMENT 1: two 80.00 withdrawals from 100.00\n";
    Fixtures f(observer, "competing", {"100.00", "0.00", "0.00"});
    auto first = connect();
    auto second = connect();
    const auto firstPid = std::to_string(PQbackendPID(first.get()));
    const auto secondPid = std::to_string(PQbackendPID(second.get()));
    Gate gate;
    Outcome a, b;
    bool sawBlocking = false;
    {
        std::jthread worker1(runWorker, first.get(), f.accounts[0], f.accounts[1], 8000,
                             std::cref(f.refs[0]), &gate, std::ref(a));
        if (!gate.awaitFirst()) {
            gate.release();
            throw std::runtime_error("First debit did not reach the test checkpoint");
        }
        std::jthread worker2(runWorker, second.get(), f.accounts[0], f.accounts[2], 8000,
                             std::cref(f.refs[1]), nullptr, std::ref(b));
        try {
            const auto deadline = std::chrono::steady_clock::now() + 4s;
            do {
                sawBlocking = scalar(observer,
                    "SELECT $1::int = ANY(pg_blocking_pids($2::int))",
                    {firstPid, secondPid}) == "t";
                if (sawBlocking) break;
                std::this_thread::sleep_for(50ms);
            } while (std::chrono::steady_clock::now() < deadline);
        } catch (...) { gate.release(); throw; }
        gate.release();
    } // joins both workers before reading their outcomes or cleaning fixtures
    show("A -> B", a); show("A -> C", b);
    const auto source = f.balance(0), recipient1 = f.balance(1), recipient2 = f.balance(2);
    std::cout << "Observed row-lock blocking=" << sawBlocking << "\nBalances in paise: "
              << source << ", " << recipient1 << ", " << recipient2 << '\n';
    const bool ok = sawBlocking && !gate.timedOut && a.success && !b.success &&
        a.sqlstate.empty() && b.sqlstate.empty() && a.exception.empty() && b.exception.empty() &&
        a.idle && b.idle && source == 2000 && recipient1 == 8000 && recipient2 == 0 &&
        source + recipient1 + recipient2 == 10000 &&
        f.ledger(0, true, 0, 1, "80.00") && f.ledger(1, false, 0, 2, "80.00");
    f.cleanup();
    std::cout << (ok ? "PASS" : "FAIL/INCONCLUSIVE") << ": competing withdrawals\n";
    return ok;
}

bool opposing(PGconn* observer) {
    std::cout << "\nEXPERIMENT 2: opposite transfers, both source debits held before credit\n";
    Fixtures f(observer, "opposing", {"100.00", "100.00"});
    auto first = connect();
    auto second = connect();
    std::cout << "deadlock_timeout=" << scalar(first.get(), "SHOW deadlock_timeout") << '\n';
    Gate gate;
    gate.target = 2;
    Outcome a, b;
    {
        std::jthread worker1(runWorker, first.get(), f.accounts[0], f.accounts[1], 1000,
                             std::cref(f.refs[0]), &gate, std::ref(a));
        std::jthread worker2(runWorker, second.get(), f.accounts[1], f.accounts[0], 1000,
                             std::cref(f.refs[1]), &gate, std::ref(b));
    }
    show("A -> B", a); show("B -> A", b);
    const auto balanceA = f.balance(0), balanceB = f.balance(1);
    std::cout << "Balances in paise: " << balanceA << ", " << balanceB << '\n';
    const bool oneVictim = (a.success && a.sqlstate.empty() && !b.success && b.sqlstate == "40P01") ||
                          (b.success && b.sqlstate.empty() && !a.success && a.sqlstate == "40P01");
    const bool ok = !gate.timedOut && gate.arrived == 2 && oneVictim &&
        a.exception.empty() && b.exception.empty() && a.idle && b.idle &&
        balanceA == (a.success ? 9000 : 11000) && balanceB == (a.success ? 11000 : 9000) &&
        balanceA + balanceB == 20000 &&
        f.ledger(0, a.success, 0, 1, "10.00") && f.ledger(1, b.success, 1, 0, "10.00");
    f.cleanup();
    std::cout << (ok ? "REPRODUCED" : "FAIL/INCONCLUSIVE")
              << ": deadlock 40P01 with one committed transfer and full victim rollback\n";
    return ok;
}
} // namespace

bool ledgercoreTestAfterDebit() { return !workerGate || workerGate->arrive(); }

void ledgercoreTestSqlError(PGresult* result) {
    if (firstSqlState.empty()) {
        if (const char* state = PQresultErrorField(result, PG_DIAG_SQLSTATE)) firstSqlState = state;
    }
}

int main() {
    try {
        auto observer = connect();
        if (scalar(observer.get(), "SELECT pg_try_advisory_lock(5315,20261106)") != "t")
            throw std::runtime_error("Another Phase 5 runner is active; stop here");
        if (scalar(observer.get(),
            "SELECT EXISTS(SELECT FROM pg_trigger WHERE tgrelid='public.journal_entries'::regclass "
            "AND tgname='trg_journal_entries_balanced' AND tgdeferrable AND tginitdeferred "
            "AND tgenabled IN ('O','A'))") != "t")
            throw std::runtime_error("Expected enabled deferred balancing trigger is missing");
        std::cout << "Phase 5 baseline experiments; isolation=READ COMMITTED. No retry or lock-order fix.\n";
        const bool first = competing(observer.get());
        const bool second = opposing(observer.get());
        std::cout << "\nSUMMARY: experiment 1=" << (first ? "PASS" : "FAIL/INCONCLUSIVE")
                  << "; experiment 2=" << (second ? "DEADLOCK REPRODUCED, ROLLBACK VERIFIED" : "FAIL/INCONCLUSIVE")
                  << "\nThis diagnoses the baseline. It does NOT certify deadlock-free concurrency.\n";
        return first && second ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "PHASE 5 STOPPED: " << e.what() << '\n';
        return 1;
    }
}
