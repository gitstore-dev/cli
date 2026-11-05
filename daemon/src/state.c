#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/state.h"
#include "gitstored/records.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
static int execute(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
static int scalar(sqlite3 *db, const char *sql, sqlite3_int64 *out) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK)
        return -1;
    int result = sqlite3_step(statement) == SQLITE_ROW ? 0 : -1;
    if (!result)
        *out = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return result;
}
int gstd_state_open(const char *path, sqlite3 **out) {
    *out = NULL;
    int fd = open(path, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat st;
    if (fd < 0)
        return -1;
    int valid = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
                st.st_uid == geteuid() && (st.st_mode & 0777) == 0600;
    close(fd);
    if (!valid)
        return -1;
    sqlite3 *db = NULL;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW | SQLITE_OPEN_EXRESCODE;
    if (sqlite3_open_v2(path, &db, flags, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, NULL);
    sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL);
    sqlite3_limit(db, SQLITE_LIMIT_ATTACHED, 0);
    sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 1048576);
    sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, 65536);
    sqlite3_limit(db, SQLITE_LIMIT_EXPR_DEPTH, 32);
    sqlite3_busy_timeout(db, 1000);
    if (gstd_records_functions(db))
        goto fail;
    sqlite3_int64 version, count, page_size;
    /* Inspect unknown stores before changing their persistent journal/schema settings. */
    if (scalar(db, "PRAGMA user_version", &version) || version < 0 || version > 5)
        goto fail;
    if (version == 0 &&
        (scalar(db, "SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'", &count) ||
         count))
        goto fail;
    if (scalar(db, "PRAGMA page_size", &page_size) || page_size != 4096)
        goto fail;
    if (execute(db, "PRAGMA foreign_keys=ON;PRAGMA journal_mode=DELETE;PRAGMA "
                    "synchronous=EXTRA;PRAGMA max_page_count=32000;PRAGMA "
                    "journal_size_limit=0;PRAGMA temp_store=MEMORY;BEGIN EXCLUSIVE;"))
        goto fail;
    if (scalar(db, "PRAGMA user_version", &version))
        goto rollback;
    if (version == 0) {
        if (scalar(db, "SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'",
                   &count) ||
            count != 0)
            goto rollback;
        if (execute(db, "CREATE TABLE daemon_store(owner_uid INTEGER NOT NULL,schema_version "
                        "INTEGER NOT NULL CHECK(schema_version=5));"))
            goto rollback;
        sqlite3_stmt *statement = NULL;
        if (sqlite3_prepare_v2(db, "INSERT INTO daemon_store VALUES(?,5)", -1, &statement, NULL) !=
            SQLITE_OK)
            goto rollback;
        sqlite3_bind_int64(statement, 1, (sqlite3_int64)geteuid());
        int result = sqlite3_step(statement);
        sqlite3_finalize(statement);
        if (result != SQLITE_DONE || gstd_records_create(db) ||
            execute(db, "PRAGMA user_version=5"))
            goto rollback;
        version = 5;
    } else if (version != 1 && version != 2 && version != 3 && version != 4 && version != 5)
        goto rollback;
    sqlite3_int64 owner;
    if (scalar(db, "SELECT count(*) FROM daemon_store", &count) || count != 1 ||
        scalar(db, "SELECT owner_uid FROM daemon_store", &owner) ||
        owner != (sqlite3_int64)geteuid() ||
        scalar(db, "SELECT schema_version FROM daemon_store", &count) || count != version)
        goto rollback;
    if (version == 1) {
        if (gstd_records_upgrade_v1(db) ||
            execute(db, "ALTER TABLE daemon_store RENAME TO daemon_store_v1;"
                        "CREATE TABLE daemon_store(owner_uid INTEGER NOT NULL,schema_version "
                        "INTEGER NOT NULL CHECK(schema_version=2));"
                        "INSERT INTO daemon_store SELECT owner_uid,2 FROM daemon_store_v1;"
                        "DROP TABLE daemon_store_v1;PRAGMA user_version=2;"))
            goto rollback;
    }
    if (version == 1)
        version = 2;
    if (version == 2) {
        if (gstd_records_upgrade_v2(db) ||
            execute(db, "ALTER TABLE daemon_store RENAME TO daemon_store_v2;"
                        "CREATE TABLE daemon_store(owner_uid INTEGER NOT NULL,schema_version "
                        "INTEGER NOT NULL CHECK(schema_version=3));"
                        "INSERT INTO daemon_store SELECT owner_uid,3 FROM daemon_store_v2;"
                        "DROP TABLE daemon_store_v2;PRAGMA user_version=3;"))
            goto rollback;
    }
    if (version == 2)
        version = 3;
    if (version == 3) {
        if (gstd_records_upgrade_v3(db) ||
            execute(db, "ALTER TABLE daemon_store RENAME TO daemon_store_v3;"
                        "CREATE TABLE daemon_store(owner_uid INTEGER NOT NULL,schema_version "
                        "INTEGER NOT NULL CHECK(schema_version=4));"
                        "INSERT INTO daemon_store SELECT owner_uid,4 FROM daemon_store_v3;"
                        "DROP TABLE daemon_store_v3;PRAGMA user_version=4;"))
            goto rollback;
    }
    if (version == 3)
        version = 4;
    if (version == 4) {
        if (gstd_records_upgrade_v4(db) ||
            execute(db, "ALTER TABLE daemon_store RENAME TO daemon_store_v4;"
                        "CREATE TABLE daemon_store(owner_uid INTEGER NOT NULL,schema_version "
                        "INTEGER NOT NULL CHECK(schema_version=5));"
                        "INSERT INTO daemon_store SELECT owner_uid,5 FROM daemon_store_v4;"
                        "DROP TABLE daemon_store_v4;PRAGMA user_version=5;"))
            goto rollback;
    }
    if (gstd_records_check(db))
        goto rollback;
    if (execute(db, "COMMIT"))
        goto fail;
    *out = db;
    return 0;
rollback:
    (void)execute(db, "ROLLBACK");
fail:
    sqlite3_close(db);
    return -1;
}
