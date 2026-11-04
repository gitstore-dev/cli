#include "gitstored/operations.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "recovery.h"
#include "support.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int count(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return -1;
    int result = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : -1;
    sqlite3_finalize(s);
    return result;
}
static int scenario(int stopping) {
    int result = 1;
    char *root = gstd_test_dir(), path[512], ids[3][65], taken[65];
    sqlite3 *db = NULL;
    json_t *components = json_pack("[s]", "git"), *message = NULL;
    CHECK(root && components);
    CHECK(snprintf(path, sizeof(path), "%s/recovery.db", root) > 0);
    CHECK(!gstd_state_open(path, &db));
    CHECK(sqlite3_exec(db,
                       "INSERT INTO profiles VALUES('core','v1','source','digest','linux-arm64')",
                       NULL, NULL, NULL) == SQLITE_OK);
    const char *names[] = {"running", "ready", "accepted"};
    for (unsigned i = 0; i < 3; i++) {
        message = json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:s}}", "version", "major", 1,
                            "minor", 0, "requestId", "r", "method", "deployment.start",
                            "idempotencyKey", names[i], "params", "deploymentId", names[i],
                            "profileId", "core", "profileVersion", "v1");
        gstd_request request;
        CHECK(message && !gstd_request_parse(message, &request));
        CHECK(gstd_accept(db, &request, components, names[i], ids[i]) == GSTD_ACCEPT_NEW);
        if (i < 2)
            CHECK(gstd_schedule_take(db, 0, taken) == 1 && !strcmp(taken, ids[i]));
        if (i == 1) {
            CHECK(
                sqlite3_exec(
                    db,
                    "UPDATE operations SET state='succeeded',result='{\"ready\":true}' WHERE "
                    "deployment_id='ready';UPDATE deployments SET "
                    "readiness='ready',observed_generation=generation WHERE deployment_id='ready'",
                    NULL, NULL, NULL) == SQLITE_OK);
            CHECK(!gstd_schedule_finish(db, ids[i]));
        }
        if (i == 1 && !stopping) {
            CHECK(gstd_schedule_observer_reserve(db, ids[i]) == 1);
            CHECK(sqlite3_exec(
                      db, "UPDATE deployments SET readiness='failed' WHERE deployment_id='ready'",
                      NULL, NULL, NULL) == SQLITE_OK);
        }
        if (i == 1 && stopping) {
            json_decref(message);
            message = json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s}}", "version", "major", 1, "minor",
                                0, "requestId", "stop", "method", "deployment.stop",
                                "idempotencyKey", "stop-ready", "params", "deploymentId", "ready");
            gstd_request stop;
            char id[65];
            CHECK(message && !gstd_request_parse(message, &stop));
            CHECK(gstd_accept(db, &stop, NULL, NULL, id) == GSTD_ACCEPT_NEW);
            CHECK(gstd_schedule_take(db, 0, taken) == 1 && !strcmp(taken, id));
            sqlite3_stmt *s = NULL;
            CHECK(sqlite3_prepare_v2(db,
                                     "UPDATE deployments SET "
                                     "desired_state='stopped',readiness='pending',last_operation_"
                                     "id=? WHERE deployment_id='ready'",
                                     -1, &s, NULL) == SQLITE_OK);
            sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
            int rc = sqlite3_step(s);
            sqlite3_finalize(s);
            CHECK(rc == SQLITE_DONE);
        }
        json_decref(message);
        message = NULL;
    }
    CHECK(!gstd_recovery_invalidate(db));
    CHECK(!gstd_recovery_invalidate(db));
    CHECK(count(db,
                "SELECT count(*) FROM operations WHERE state='failed' AND recovery='uncertain' AND "
                "error='{\"code\":\"OWNERSHIP_UNCERTAIN\",\"stage\":\"recovery\",\"retryable\":"
                "false}'") == (stopping ? 2 : 1));
    CHECK(count(db, "SELECT count(*) FROM operations WHERE state='succeeded' AND "
                    "result='{\"ready\":true}'") == 1);
    CHECK(count(db, "SELECT count(*) FROM operations WHERE state='accepted'") == 1);
    CHECK(count(db, "SELECT count(*) FROM deployments WHERE readiness='ready'") == 0);
    CHECK(count(db, "SELECT count(*) FROM resource_ownership WHERE ownership_state='uncertain'") ==
          2);
    CHECK(count(db, "SELECT count(*) FROM resource_ownership WHERE ownership_state='held'") == 1);
    CHECK(count(db, "SELECT count(*) FROM jobs WHERE state='running'") == 0);
    CHECK(gstd_schedule_take(db, 0, taken) == 1 && !strcmp(taken, ids[2]));
    CHECK(!gstd_recovery_invalidate(db));
    CHECK(count(db, "SELECT count(*) FROM operations WHERE state='failed'") == (stopping ? 3 : 2));
    CHECK(count(db, "SELECT count(*) FROM resource_ownership WHERE ownership_state='released'") ==
          0);
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

int main(void) { return scenario(0) || scenario(1); }
