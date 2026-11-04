#ifndef GSTD_TEST_FAILPOINTS_H
#define GSTD_TEST_FAILPOINTS_H
#include <sqlite3.h>
/* Test executable only. Install after opening a fixture store. The next COMMIT
 * kills the database-owning process before SQLite executes the statement. */
int gstd_test_crash_before_commit(sqlite3 *);
#endif
