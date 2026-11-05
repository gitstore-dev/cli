#define _POSIX_C_SOURCE 200809L
#include "restart.h"
#include "gitstored/execution.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int snapshot(sqlite3 *db, const char *operation, int stopped, json_t **intent) {
    sqlite3_stmt *s = NULL;
    const char *sql =
        "SELECT o.state,o.intent,d.readiness FROM operations o JOIN deployments d ON "
        "d.last_operation_id=o.operation_id WHERE o.operation_id=? AND o.caller_uid=? "
        "AND o.recovery='normal' AND d.recovery='normal' AND "
        "((?=0 AND o.method='deployment.start' AND d.desired_state='running') OR "
        "(?=1 AND o.method='deployment.stop' AND d.desired_state='stopped' AND "
        "d.writer_ownership='released' AND d.observed_generation=d.generation AND NOT EXISTS("
        "SELECT 1 FROM resource_ownership r WHERE r.deployment_id=d.deployment_id AND "
        "r.ownership_state!='released'))) AND NOT EXISTS(SELECT 1 FROM operations active "
        "WHERE active.deployment_id=d.deployment_id AND active.operation_id!=o.operation_id "
        "AND active.state IN ('accepted','running'))";
    if (!db || !operation || sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
    sqlite3_bind_int(s, 3, stopped);
    sqlite3_bind_int(s, 4, stopped);
    int step = sqlite3_step(s), result = -1;
    if (step == SQLITE_ROW) {
        const char *state = (const char *)sqlite3_column_text(s, 0);
        if (state && (!strcmp(state, "accepted") || !strcmp(state, "running")))
            result = 0;
        else if (state && !strcmp(state, "succeeded")) {
            result = 1;
            if (intent) {
                const unsigned char *bytes = sqlite3_column_text(s, 1);
                int count = sqlite3_column_bytes(s, 1);
                *intent = bytes && count > 0 ? gstd_json_parse(bytes, (size_t)count) : NULL;
                if (!*intent)
                    result = -1;
            }
        }
    }
    sqlite3_finalize(s);
    return result;
}
void gstd_restart_clear(gstd_restart *r) {
    if (r) {
        json_decref(r->intent);
        memset(r, 0, sizeof(*r));
    }
}
int gstd_restart_departed(sqlite3 *db, const char *start, gstd_restart *r,
                          gstd_restart_submit submit, void *context) {
    if (!db || !start || !r || !submit || !gstd_identifier(start, strnlen(start, 65)) ||
        (r->phase && r->phase != 1 && r->phase != 4))
        return -1;
    if (r->phase && strcmp(r->current, start))
        return -1;
    json_t *intent = NULL;
    if (snapshot(db, start, 0, &intent) != 1) {
        json_decref(intent);
        return -1;
    }
    const char *deployment = json_string_value(json_object_get(intent, "deploymentId"));
    if (!deployment || !gstd_identifier(deployment, strlen(deployment)) ||
        json_object_set_new(intent, "allowProvision", json_false())) {
        json_decref(intent);
        return -1;
    }
    if (!r->phase) {
        memcpy(r->deployment, deployment, strlen(deployment) + 1);
        memcpy(r->current, start, strlen(start) + 1);
        r->intent = intent;
    } else
        json_decref(intent);
    /* Budget exhaustion never starts a new cleanup/replacement cycle. Existing
     * failed readiness and owned resources remain available for explicit stop. */
    unsigned delay;
    if (gstd_execution_backoff(r->attempts + 1, r->delayed_ms, &delay) != 1) {
        r->phase = 5;
        return 0;
    }
    json_t *params =
        json_pack("{s:s,s:s}", "deploymentId", r->deployment, "expectedOperationId", r->current);
    char key[65];
    snprintf(key, sizeof(key), "auto-stop-%.54s", r->current);
    r->phase = 5; /* Fail closed if acceptance is rejected or cannot be observed. */
    int accepted = params && !submit(context, GSTD_STOP, params, key, r->stop);
    json_decref(params);
    if (!accepted)
        return -1;
    r->attempts++;
    r->phase = 2;
    return 0;
}
void gstd_restart_poll(sqlite3 *db, gstd_restart *r, int64_t now, gstd_restart_submit submit,
                       void *context) {
    if (!r || !r->phase || r->phase == 5)
        return;
    if (!db || !submit || now < 0) {
        r->phase = 5;
        return;
    }
    if (r->phase == 1) {
        if (snapshot(db, r->current, 0, NULL) != 1)
            r->phase = 5;
        return;
    }
    if (r->phase == 4) {
        int state = snapshot(db, r->current, 0, NULL);
        if (state)
            r->phase = state == 1 ? 1 : 5;
        return;
    }
    /* A still-running stop legitimately retains owners. Distinguish pending
     * from failed/superseded without treating released metadata as termination. */
    if (r->phase == 2) {
        sqlite3_stmt *s = NULL;
        int pending = 0;
        if (sqlite3_prepare_v2(db,
                               "SELECT 1 FROM operations o JOIN deployments d ON "
                               "d.deployment_id=o.deployment_id WHERE o.operation_id=? "
                               "AND o.method='deployment.stop' AND o.caller_uid=? AND "
                               "o.state IN ('accepted','running') AND "
                               "d.last_operation_id IN (o.operation_id,?) "
                               "AND o.recovery='normal' AND d.recovery!='uncertain'",
                               -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, r->stop, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
            sqlite3_bind_text(s, 3, r->current, -1, SQLITE_TRANSIENT);
            pending = sqlite3_step(s) == SQLITE_ROW;
        }
        sqlite3_finalize(s);
        if (pending)
            return;
        unsigned delay = 0;
        if (snapshot(db, r->stop, 1, NULL) != 1 ||
            gstd_execution_backoff(r->attempts, r->delayed_ms, &delay) != 1 ||
            now > INT64_MAX - delay) {
            r->phase = 5;
            return;
        }
        r->delayed_ms += delay;
        r->due_ms = now + delay;
        r->phase = 3;
    }
    if (r->phase != 3)
        return;
    if (snapshot(db, r->stop, 1, NULL) != 1) {
        r->phase = 5;
        return;
    }
    if (now < r->due_ms)
        return;
    char key[65], replacement[65];
    snprintf(key, sizeof(key), "auto-start-%.53s", r->stop);
    r->phase = 5;
    if (!submit(context, GSTD_START, r->intent, key, replacement)) {
        memcpy(r->current, replacement, strlen(replacement) + 1);
        r->phase = 4;
    }
}
