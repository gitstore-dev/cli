#define _POSIX_C_SOURCE 200809L
#include "failpoints.h"
#include <signal.h>
#include <string.h>
#include <unistd.h>
static int before_commit(unsigned kind, void *context, void *statement, void *sql) {
    (void)context;
    (void)statement;
    if (kind == SQLITE_TRACE_STMT && sql && !strcmp(sql, "COMMIT"))
        kill(getpid(), SIGKILL);
    return 0;
}
int gstd_test_crash_before_commit(sqlite3 *db) {
    return db ? sqlite3_trace_v2(db, SQLITE_TRACE_STMT, before_commit, NULL) : SQLITE_MISUSE;
}
