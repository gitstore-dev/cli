#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/state.h"
#include "support.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int integer(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return -1;
    int n = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : -1;
    sqlite3_finalize(s);
    return n;
}
int main(void) {
    int result = 0;
    char *root = gstd_test_dir();
    sqlite3 *db = NULL;
    char path[256], link[256];
    CHECK(root);
    CHECK(snprintf(path, sizeof(path), "%s/state.db", root) > 0);
    CHECK(!gstd_state_open(path, &db));
    CHECK(integer(db, "PRAGMA max_page_count") == 32000 && integer(db, "PRAGMA page_size") == 4096);
    CHECK(integer(db, "PRAGMA user_version") == 6 && integer(db, "PRAGMA synchronous") == 3 &&
          integer(db, "PRAGMA foreign_keys") == 1);
    CHECK(sqlite3_exec(db, "CREATE TABLE t(n);BEGIN;INSERT INTO t VALUES(1);ROLLBACK;", NULL, NULL,
                       NULL) == SQLITE_OK);
    CHECK(integer(db, "SELECT count(*) FROM t") == 0);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    CHECK(!gstd_state_open(path, &db));
    CHECK(sqlite3_exec(db, "UPDATE daemon_store SET owner_uid=owner_uid+1", NULL, NULL, NULL) ==
          SQLITE_OK);
    sqlite3_close(db);
    db = NULL;
    CHECK(gstd_state_open(path, &db) != 0 && db == NULL);
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "UPDATE daemon_store SET owner_uid=owner_uid-1", NULL, NULL, NULL) ==
          SQLITE_OK);
    CHECK(sqlite3_exec(db, "PRAGMA user_version=7", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    db = NULL;
    CHECK(gstd_state_open(path, &db) != 0 && db == NULL);
    CHECK(sqlite3_open(path, &db) == SQLITE_OK && integer(db, "PRAGMA user_version") == 7);
    sqlite3_close(db);
    db = NULL;
    CHECK(snprintf(link, sizeof(link), "%s/link.db", root) > 0);
    CHECK(!symlink(path, link));
    CHECK(gstd_state_open(link, &db) != 0 && db == NULL);
    CHECK(!chmod(path, 0644));
    CHECK(gstd_state_open(path, &db) != 0 && db == NULL);
done:
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
