#define _POSIX_C_SOURCE 200809L
#include "gitstored/operations.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "support.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "read slots line %d: %s\n", __LINE__, #x);                             \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        value = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return value;
}
int main(void) {
    gstd_json_initialize();
    int result = 1;
    char *root = gstd_test_dir(), path[512], ids[8][65], taken[65];
    sqlite3 *db = NULL;
    json_t *message = NULL, *components = json_pack("[s]", "api");
    CHECK(root && components);
    snprintf(path, sizeof(path), "%s/state.db", root);
    CHECK(!gstd_state_open(path, &db));
    CHECK(sqlite3_exec(
              db,
              "INSERT INTO profiles VALUES('core','1','source','digest','linux-arm64');"
              "INSERT INTO deployments "
              "VALUES('observed','core','1','running',1,0,'pending','normal','released','2000-"
              "01-01T00:00:00Z',NULL)",
              NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_schedule_read_reserve(db, "reader-1", "observed") == 1);
    CHECK(gstd_schedule_read_reserve(db, "reader-2", "observed") == 1);
    CHECK(gstd_schedule_read_reserve(db, "reader-3", "observed") == 0);
    CHECK(gstd_schedule_read_reserve(db, "invalid/reader", "observed") == -1);
    CHECK(gstd_schedule_read_finish(db, "unknown") == 0);
    for (unsigned i = 0; i < 8; i++) {
        char name[32];
        snprintf(name, sizeof(name), "deployment-%u", i);
        message = json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:s}}", "version", "major", 1,
                            "minor", 0, "requestId", "request", "method", "deployment.start",
                            "idempotencyKey", name, "params", "deploymentId", name, "profileId",
                            "core", "profileVersion", "1");
        gstd_request request;
        CHECK(message && !gstd_request_parse(message, &request));
        CHECK(gstd_accept(db, &request, components, NULL, ids[i]) == GSTD_ACCEPT_NEW);
        json_decref(message);
        message = NULL;
    }
    CHECK(scalar(db, "SELECT count(*) FROM jobs WHERE worker_slot IS NOT NULL") == 6);
    CHECK(scalar(db, "SELECT count(*) FROM jobs WHERE state='queued'") == 2);
    CHECK(scalar(db, "SELECT count(*) FROM jobs j JOIN observer_slots s USING(worker_slot)") == 0);
    CHECK(sqlite3_exec(
              db, "UPDATE observer_slots SET subscription_id='replacement' WHERE worker_slot=0",
              NULL, NULL, NULL) != SQLITE_OK);
    CHECK(sqlite3_exec(db, "UPDATE jobs SET worker_slot=0 WHERE rowid=1", NULL, NULL, NULL) !=
          SQLITE_OK);
    /* A cancelled queued request must not consume the newly freed read slot. */
    sqlite3_stmt *statement = NULL;
    CHECK(sqlite3_prepare_v2(
              db, "UPDATE operations SET state='cancelled',error='{}' WHERE operation_id=?", -1,
              &statement, NULL) == SQLITE_OK);
    sqlite3_bind_text(statement, 1, ids[6], -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    CHECK(rc == SQLITE_DONE);
    CHECK(!gstd_schedule_read_finish(db, "reader-1"));
    CHECK(scalar(db, "SELECT count(*) FROM jobs WHERE worker_slot=0") == 1);
    CHECK(scalar(db, "SELECT count(*) FROM jobs j JOIN operations o USING(operation_id) WHERE "
                     "worker_slot=0 AND o.state='accepted'") == 1);
    CHECK(gstd_schedule_read_reserve(db, "reader-3", "observed") == 0);
    /* Reopening alone retains the in-flight lease. Only manager startup recovery
     * under the store lifetime lock may release a previous owner's reader. */
    CHECK(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    CHECK(!gstd_state_open(path, &db));
    CHECK(scalar(db, "SELECT count(*) FROM observer_slots") == 1);
    CHECK(!gstd_schedule_read_recover(db));
    CHECK(scalar(db, "SELECT count(*) FROM observer_slots") == 0);
    CHECK(scalar(db, "SELECT count(*) FROM operations WHERE state='cancelled' AND error='{}'") ==
          1);
    CHECK(gstd_schedule_read_reserve(db, "reader-3", "observed") == 1);
    CHECK(!gstd_schedule_read_finish(db, "reader-3"));
    CHECK(!gstd_schedule_read_recover(db));
    CHECK(gstd_schedule_take(db, 0, taken) == 1 && !strcmp(taken, ids[0]));
    result = 0;
done:
    json_decref(message);
    json_decref(components);
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
