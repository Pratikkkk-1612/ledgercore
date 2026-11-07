#pragma once

#include <libpq-fe.h>
#include <string>

bool transfer(PGconn* conn, long long sourceAccountId,
              long long destinationAccountId, long long amountPaise,
              const std::string& reference, bool injectFailure = false,
              bool* expectedFailureObserved = nullptr);
