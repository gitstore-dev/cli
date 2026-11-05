#define _POSIX_C_SOURCE 200809L
#include "cancellation.h"
#include "gitstored/inspect.h"
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
static int deny_after_dispatch(void *data, int action, const char *table, const char *column,
                               const char *database, const char *origin) {
    (void)database;
    (void)origin;
    unsigned *updates = data;
    if (action == SQLITE_UPDATE && table && column && !strcmp(table, "operations") &&
        !strcmp(column, "state") && ++*updates > 1)
        return SQLITE_DENY;
    return SQLITE_OK;
}
static int deny_job_completion(void *data, int action, const char *table, const char *column,
                               const char *database, const char *origin) {
    (void)column;
    (void)database;
    (void)origin;
    unsigned *updates = data;
    if (action == SQLITE_UPDATE && table && !strcmp(table, "jobs") && ++*updates > 1)
        return SQLITE_DENY;
    return SQLITE_OK;
}
int main(void) {
    gstd_json_initialize();
    gstd_deployment actor = {.count = 3};
    CHECK(!gstd_cancellation_can_notify(NULL));
    CHECK(gstd_cancellation_can_notify(&actor));
    actor.bootstrap.worker = 123;
    actor.graph.stage = GSTD_BOOT_ENROLL;
    CHECK(!gstd_cancellation_can_notify(&actor));
    actor.bootstrap.worker = 0;
    actor.native_job.worker = 123;
    CHECK(!gstd_cancellation_can_notify(&actor));
    actor.workloads[0].native.expected_pid = 456;
    CHECK(gstd_cancellation_can_notify(&actor));
    actor.index = 3;
    CHECK(!gstd_cancellation_can_notify(&actor));
    actor.native_job.worker = 0;
    CHECK(gstd_cancellation_can_notify(&actor)); /* Volume/network preparation. */
    actor.graph.persistent = 1;
    actor.graph.stage = GSTD_BOOT_PREPARE_KEYSPACE;
    CHECK(!gstd_cancellation_can_notify(&actor));
    actor.graph.stage = GSTD_BOOT_MIGRATE_SCHEMA;
    CHECK(!gstd_cancellation_can_notify(&actor));
    actor.graph.stage = GSTD_BOOT_WAIT_DATASTORE;
    CHECK(gstd_cancellation_can_notify(&actor));
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
    /* Cancellation has durable identity independent of its target and survives
     * manager-store reopen before dispatch, with identical-key replay. */
    CHECK(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    CHECK(!gstd_state_open(path, &db));
    char replay[65];
    CHECK(accept(db, "operation.cancel", "cancel-queued", params, NULL, NULL, replay) ==
          GSTD_ACCEPT_REPLAY);
    CHECK(!strcmp(replay, cancelled));
    unsigned job_updates = 0;
    CHECK(sqlite3_set_authorizer(db, deny_job_completion, &job_updates) == SQLITE_OK);
    gstd_test_control_once(db);
    CHECK(state(db, starts[8], "cancelled", NULL) && state(db, cancelled, "succeeded", NULL));
    CHECK(count(db,
                "SELECT count(*) FROM jobs j JOIN operations o ON o.operation_id=j.operation_id "
                "WHERE j.state!='done' AND o.state IN ('cancelled','succeeded')") == 2);
    CHECK(sqlite3_set_authorizer(db, NULL, NULL) == SQLITE_OK);
    gstd_test_control_once(db);
    gstd_test_control_once(db);
    CHECK(count(db,
                "SELECT count(*) FROM jobs j JOIN operations o ON o.operation_id=j.operation_id "
                "WHERE j.state!='done' AND o.state IN ('cancelled','succeeded')") == 0);
    CHECK(count(db, "SELECT count(*) FROM jobs WHERE kind='runtime' AND state='reserved'") == 8);
    CHECK(count(db, "SELECT count(*) FROM resource_ownership WHERE deployment_id='dep-8' AND "
                    "ownership_state='released'") == 1);
    CHECK(count(db, "SELECT count(*) FROM resource_ownership WHERE deployment_id!='dep-8' AND "
                    "ownership_state='held'") == 8);
    CHECK(sqlite3_exec(db, "DELETE FROM jobs WHERE state='done' AND kind='runtime'", NULL, NULL,
                       NULL) == SQLITE_OK);
    CHECK(accept(db, "operation.cancel", "cancel-terminal", params, NULL, NULL, noop) ==
          GSTD_ACCEPT_NEW);
    json_decref(params);
    unsigned updates = 0;
    CHECK(sqlite3_set_authorizer(db, deny_after_dispatch, &updates) == SQLITE_OK);
    gstd_test_control_once(db);
    CHECK(state(db, noop, "running", NULL));
    CHECK(state(db, starts[8], "cancelled", NULL));
    CHECK(sqlite3_set_authorizer(db, NULL, NULL) == SQLITE_OK);
    gstd_test_control_once(db);
    CHECK(state(db, noop, "succeeded", NULL));
    CHECK(count(db, "SELECT count(*) FROM operations WHERE json_extract(result,'$.noOp')=1") == 1);
    CHECK(count(db, "SELECT count(*) FROM operations WHERE json_extract(result,'$.noOp')=1 "
                    "AND json_extract(result,'$.targetState')='cancelled'") == 1);
    json_t *selected = json_pack("{s:s}", "operationId", noop), *inspected = NULL;
    CHECK(selected &&
          gstd_inspect(db, GSTD_OPERATION_INSPECT, selected, &inspected) == GSTD_QUERY_OK);
    CHECK(!strcmp(
        json_string_value(json_object_get(json_object_get(inspected, "result"), "targetState")),
        "cancelled"));
    json_decref(selected);
    json_decref(inspected);
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
    /* Running controls already mapped to actors cannot be dispatched twice;
     * another accepted control must still progress. Terminal runtime jobs are
     * deliberately left alone: a running row can be a live health lease. */
    char first_control[65], next_control[65], selected_control[65];
    params = json_pack("{s:s}", "operationId", replacement);
    CHECK(accept(db, "operation.cancel", "mapped-control", params, NULL, NULL, first_control) ==
          GSTD_ACCEPT_NEW);
    CHECK(gstd_schedule_take(db, 1, selected_control) == 1 &&
          !strcmp(selected_control, first_control));
    CHECK(accept(db, "operation.cancel", "other-control", params, NULL, NULL, next_control) ==
          GSTD_ACCEPT_NEW);
    json_decref(params);
    const char *waiting[] = {first_control};
    CHECK(gstd_cancellation_next(db, waiting, 1, selected_control) == 1 &&
          !strcmp(selected_control, next_control));
    CHECK(state(db, first_control, "running", NULL));
    gstd_test_control_once(db);
    CHECK(state(db, first_control, "succeeded", NULL));
    gstd_test_control_once(db);
    CHECK(state(db, next_control, "succeeded", NULL));
    CHECK(count(db, "SELECT count(*) FROM operations WHERE json_extract(result,'$.noOp')=1 AND "
                    "json_extract(result,'$.targetState')='succeeded'") == 2);
    params = json_pack("{s:s}", "operationId", unsafe);
    CHECK(accept(db, "operation.cancel", "failed-noop", params, NULL, NULL, selected_control) ==
          GSTD_ACCEPT_NEW);
    json_decref(params);
    gstd_test_control_once(db);
    CHECK(state(db, selected_control, "succeeded", NULL) &&
          state(db, unsafe, "failed", "UNSAFE_CANCELLATION"));
    CHECK(count(db, "SELECT count(*) FROM operations WHERE json_extract(result,'$.noOp')=1 AND "
                    "json_extract(result,'$.targetState')='failed'") == 1);
    json_decref(components);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    CHECK(!gstd_test_remove(root));
    return 0;
}
