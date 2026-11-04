#define _POSIX_C_SOURCE 200809L
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "support.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
void gstd_test_control_once(sqlite3 *);
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "control line %d: %s\n", __LINE__, #x);                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int state(sqlite3 *db, const char *id, const char *expected, const char *error) {
    sqlite3_stmt *s = NULL;
    int ok =
        sqlite3_prepare_v2(
            db, "SELECT state,json_extract(error,'$.code') FROM operations WHERE operation_id=?",
            -1, &s, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(s) == SQLITE_ROW;
        if (ok) {
            const char *value = (const char *)sqlite3_column_text(s, 0),
                       *code = (const char *)sqlite3_column_text(s, 1);
            ok = value && !strcmp(value, expected) && (!error || (code && !strcmp(code, error)));
        }
    }
    sqlite3_finalize(s);
    return ok;
}
static int count(sqlite3 *db, const char *query) {
    sqlite3_stmt *s = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(db, query, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        value = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return value;
}
static gstd_accept_status accept(sqlite3 *db, const char *method, const char *key, json_t *params,
                                 const json_t *components, const char *volume, char operation[65]) {
    json_t *message =
        json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:O}", "version", "major", 1, "minor", 0, "requestId",
                  "fixture", "method", method, "idempotencyKey", key, "params", params);
    gstd_request request;
    gstd_accept_status result = message && !gstd_request_parse(message, &request)
                                    ? gstd_accept(db, &request, components, volume, operation)
                                    : GSTD_ACCEPT_INVALID;
    json_decref(message);
    return result;
}
int main(void) {
    gstd_json_initialize();
    char *root = gstd_test_dir(), path[4096], starts[9][65], cancelled[65], noop[65], unsafe[65];
    CHECK(root);
    snprintf(path, sizeof(path), "%s/state.db", root);
    sqlite3 *db = NULL;
    CHECK(!gstd_state_open(path, &db));
    CHECK(sqlite3_exec(
              db, "INSERT INTO profiles VALUES('core','1','controlled','controlled','linux-arm64')",
              NULL, NULL, NULL) == SQLITE_OK);
    json_t *components = json_pack("[s,s]", "api", "git");
    CHECK(components);
    for (unsigned i = 0; i < 9; i++) {
        char id[32], key[32], volume[32];
        snprintf(id, sizeof(id), "dep-%u", i);
        snprintf(key, sizeof(key), "start-%u", i);
        snprintf(volume, sizeof(volume), "volume-%u", i);
        json_t *params = json_pack("{s:s,s:s,s:s}", "deploymentId", id, "profileId", "core",
                                   "profileVersion", "1");
        CHECK(accept(db, "deployment.start", key, params, components, volume, starts[i]) ==
              GSTD_ACCEPT_NEW);
        json_decref(params);
    }
    CHECK(count(db, "SELECT count(*) FROM jobs WHERE kind='runtime' AND state='reserved'") == 8);
    CHECK(count(db, "SELECT count(*) FROM jobs WHERE kind='runtime' AND state='queued'") == 1);
    json_t *params = json_pack("{s:s}", "operationId", starts[8]);
    CHECK(params);
    CHECK(accept(db, "operation.cancel", "cancel-queued", params, NULL, NULL, cancelled) ==
          GSTD_ACCEPT_NEW);
    gstd_test_control_once(db);
    CHECK(state(db, starts[8], "cancelled", NULL) && state(db, cancelled, "succeeded", NULL));
    CHECK(count(db, "SELECT count(*) FROM jobs WHERE kind='runtime' AND state='reserved'") == 8);
    CHECK(count(db, "SELECT count(*) FROM resource_ownership WHERE deployment_id='dep-8' AND "
                    "ownership_state='released'") == 1);
    CHECK(count(db, "SELECT count(*) FROM resource_ownership WHERE deployment_id!='dep-8' AND "
                    "ownership_state='held'") == 8);
    CHECK(accept(db, "operation.cancel", "cancel-terminal", params, NULL, NULL, noop) ==
          GSTD_ACCEPT_NEW);
    json_decref(params);
    gstd_test_control_once(db);
    CHECK(state(db, noop, "succeeded", NULL));
    CHECK(count(db, "SELECT count(*) FROM operations WHERE json_extract(result,'$.noOp')=1") == 1);
    char running[65];
    CHECK(gstd_schedule_take(db, 0, running) == 1);
    params = json_pack("{s:s}", "operationId", running);
    CHECK(accept(db, "operation.cancel", "unknown-active", params, NULL, NULL, unsafe) ==
          GSTD_ACCEPT_NEW);
    json_decref(params);
    gstd_test_control_once(db);
    CHECK(state(db, unsafe, "failed", "UNSAFE_CANCELLATION"));
    CHECK(state(db, running, "running", NULL));
    /* A stale foreground cannot stop a newer target. All operations here are
     * controlled metadata fixtures; no workload/backend is launched. */
    params = json_pack("{s:s,s:s,s:s}", "deploymentId", "dep-8", "profileId", "core",
                       "profileVersion", "1");
    char replacement[65];
    CHECK(accept(db, "deployment.start", "replacement", params, components, "volume-8",
                 replacement) == GSTD_ACCEPT_NEW);
    json_decref(params);
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(db,
                             "UPDATE operations SET state=CASE WHEN state='accepted' THEN "
                             "'running' ELSE 'succeeded' END WHERE operation_id=?",
                             -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, replacement, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(s) == SQLITE_DONE);
    CHECK(sqlite3_reset(s) == SQLITE_OK);
    CHECK(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    int before = count(db, "SELECT count(*) FROM operations");
    params = json_pack("{s:s,s:s}", "deploymentId", "dep-8", "expectedOperationId", starts[8]);
    char rejected[65];
    CHECK(accept(db, "deployment.stop", "stale-foreground", params, NULL, NULL, rejected) ==
          GSTD_ACCEPT_DEPLOYMENT_CONFLICT);
    json_decref(params);
    CHECK(count(db, "SELECT count(*) FROM operations") == before);
    CHECK(count(db, "SELECT count(*) FROM deployments WHERE deployment_id='dep-8' AND "
                    "writer_ownership='held' AND generation=2") == 1);
    json_decref(components);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    CHECK(!gstd_test_remove(root));
    return 0;
}
