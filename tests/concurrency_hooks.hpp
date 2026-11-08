#pragma once

#include <libpq-fe.h>

// Compiled only into the experiment executable, never the normal application.
// Returning false aborts a test whose synchronization deadline was missed.
bool ledgercoreTestAfterAccountLocks();
void ledgercoreTestSqlError(PGresult* result);
