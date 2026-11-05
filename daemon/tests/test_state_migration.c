#define _POSIX_C_SOURCE 200809L
#include "datastore_schema.inc"
#include "gitstored/records.h"
#include "gitstored/state.h"
#include "migration_network_schema.inc"
#include "observer_schema.inc"
#include "schema.inc"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "state migration line %d: %s\n", __LINE__, #x);                        \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int integer(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        value = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return value;
}
static int legacy(const char *path, int damaged, int version) {
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return -1;
    close(fd);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    int ok =
        !gstd_records_functions(db) && sqlite3_exec(db, schema_sql, NULL, NULL, NULL) == SQLITE_OK;
    char *sql = sqlite3_mprintf(
        "CREATE TABLE daemon_store(owner_uid INTEGER NOT NULL,schema_version INTEGER NOT NULL "
        "CHECK(schema_version=1));"
        "INSERT INTO daemon_store VALUES(%lld,1);"
        "INSERT INTO profiles VALUES('core','1','controlled','controlled','linux-arm64');"
        "INSERT INTO deployments "
        "VALUES('kept','core','1','running',1,0,'pending','uncertain','held','2000-01-01T00:00:00Z'"
        ",NULL);"
        "INSERT INTO operations "
        "VALUES('old-start',%lld,'deployment.start','kept',NULL,printf('%%064d',0),'{}','2000-01-"
        "01T00:00:00Z','2000-01-01T00:00:00Z','succeeded','uncertain','{}',NULL);"
        "INSERT INTO dedupe "
        "VALUES(%lld,'deployment.start','original-key',printf('%%064d',0),'old-start',9999999999);"
        "INSERT INTO resource_ownership "
        "VALUES('volume','repository-volume','kept','old-start','reserved',NULL,'held','2000-01-"
        "01T00:00:00Z');"
        "PRAGMA user_version=1;",
        (sqlite3_int64)geteuid(), (sqlite3_int64)geteuid(), (sqlite3_int64)geteuid());
    ok = ok && sql && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_free(sql);
    if (ok && version >= 2)
        ok = sqlite3_exec(db, observer_schema_sql, NULL, NULL, NULL) == SQLITE_OK &&
             sqlite3_exec(db,
                          "ALTER TABLE daemon_store RENAME TO legacy_store;"
                          "CREATE TABLE daemon_store(owner_uid INTEGER NOT NULL,schema_version "
                          "INTEGER NOT NULL CHECK(schema_version=2));"
                          "INSERT INTO daemon_store SELECT owner_uid,2 FROM legacy_store;DROP "
                          "TABLE legacy_store;PRAGMA user_version=2;",
                          NULL, NULL, NULL) == SQLITE_OK;
    if (ok && version >= 3)
        ok = sqlite3_exec(db, datastore_schema_sql, NULL, NULL, NULL) == SQLITE_OK &&
             sqlite3_exec(db,
                          "ALTER TABLE daemon_store RENAME TO legacy_store;"
                          "CREATE TABLE daemon_store(owner_uid INTEGER NOT NULL,schema_version "
                          "INTEGER NOT NULL CHECK(schema_version=3));"
                          "INSERT INTO daemon_store SELECT owner_uid,3 FROM legacy_store;DROP "
                          "TABLE legacy_store;PRAGMA user_version=3;",
                          NULL, NULL, NULL) == SQLITE_OK;
    if (ok && version >= 4)
        ok = sqlite3_exec(db, migration_network_schema_sql, NULL, NULL, NULL) == SQLITE_OK &&
             sqlite3_exec(db,
                          "ALTER TABLE daemon_store RENAME TO legacy_store;"
                          "CREATE TABLE daemon_store(owner_uid INTEGER NOT NULL,schema_version "
                          "INTEGER NOT NULL CHECK(schema_version=4));"
                          "INSERT INTO daemon_store SELECT owner_uid,4 FROM legacy_store;DROP "
                          "TABLE legacy_store;PRAGMA user_version=4;",
                          NULL, NULL, NULL) == SQLITE_OK;
    if (ok && damaged == 1)
        ok = sqlite3_exec(db, "DROP TRIGGER terminal_outcome_immutable", NULL, NULL, NULL) ==
             SQLITE_OK;
    sqlite3_close(db);
    return ok ? 0 : -1;
}
static int scenario(int damaged, int version) {
    int result = 1;
    char *root = gstd_test_dir(), path[512];
    sqlite3 *db = NULL;
    CHECK(root);
    snprintf(path, sizeof(path), "%s/state.db", root);
    CHECK(!legacy(path, damaged, version));
    if (damaged == 2) {
        CHECK(!chmod(root, 0500));
        CHECK(gstd_state_open(path, &db) == -1 && !db);
        CHECK(!chmod(root, 0700));
        CHECK(sqlite3_open(path, &db) == SQLITE_OK);
        CHECK(integer(db, "PRAGMA user_version") == version);
        CHECK(integer(db, "SELECT count(*) FROM sqlite_schema WHERE name='datastore_volumes'") ==
              (version >= 3));
        CHECK(sqlite3_close(db) == SQLITE_OK);
        db = NULL;
    }
    if (damaged == 1) {
        CHECK(gstd_state_open(path, &db) == -1 && !db);
        CHECK(sqlite3_open(path, &db) == SQLITE_OK);
        CHECK(integer(db, "PRAGMA user_version") == version);
        CHECK(integer(db, "SELECT count(*) FROM sqlite_schema WHERE name='observer_slots'") ==
              (version >= 2));
        CHECK(integer(db, "SELECT count(*) FROM sqlite_schema WHERE name='datastore_volumes'") ==
              (version >= 3));
    } else {
        CHECK(!gstd_state_open(path, &db));
        CHECK(integer(db, "PRAGMA user_version") == 5 &&
              integer(db, "SELECT schema_version FROM daemon_store") == 5);
        CHECK(integer(db, "SELECT count(*) FROM observer_slots") == 0);
        CHECK(integer(db, "SELECT count(*) FROM datastore_volumes") == 0);
        CHECK(integer(db, "SELECT count(*) FROM oidc_provider_volumes") == 0);
        CHECK(integer(db, "SELECT count(*) FROM operations WHERE operation_id='old-start' AND "
                          "state='succeeded' AND result='{}' AND error IS NULL") == 1);
        CHECK(integer(db, "SELECT count(*) FROM dedupe WHERE idempotency_key='original-key' AND "
                          "operation_id='old-start'") == 1);
        CHECK(integer(db, "SELECT count(*) FROM resource_ownership WHERE resource_id='volume' AND "
                          "ownership_state='held'") == 1);
        CHECK(sqlite3_close(db) == SQLITE_OK);
        db = NULL;
        CHECK(!gstd_state_open(path, &db) && integer(db, "PRAGMA user_version") == 5);
    }
    result = 0;
done:
    if (db)
        sqlite3_close(db);
    if (root) {
        (void)chmod(root, 0700);
        gstd_test_remove(root);
    }
    return result;
}
int main(void) {
    for (int version = 1; version <= 4; version++)
        for (int failure = 0; failure < 3; failure++)
            if (scenario(failure, version)) {
                fprintf(stderr, "legacy version=%d failure=%d\n", version, failure);
                return 1;
            }
    return 0;
}
