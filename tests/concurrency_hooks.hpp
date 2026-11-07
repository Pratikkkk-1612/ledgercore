#pragma once

#include <libpq-fe.h>

// Compiled only into the experiment executable, never the normal application.
// Returning false aborts a test whose synchronization deadline was missed.
bool ledgercoreTestAfterDebit();
void ledgercoreTestSqlError(PGresult* result);
