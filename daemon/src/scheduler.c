#include "gitstored/scheduler.h"
#include "backend.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return NULL;
    return s;
}
static int execute(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
static int finish(sqlite3_stmt *s) {
    if (!s)
        return -1;
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}
static int free_slot(sqlite3 *db) {
    for (int slot = 0; slot < 8; ++slot) {
        sqlite3_stmt *s = prepare(db, "SELECT 1 FROM jobs WHERE worker_slot=?");
        if (!s)
            return -2;
        sqlite3_bind_int(s, 1, slot);
        int rc = sqlite3_step(s);
        sqlite3_finalize(s);
        if (rc == SQLITE_DONE)
            return slot;
        if (rc != SQLITE_ROW)
            return -2;
    }
    return -1;
}
int gstd_schedule_reserve(sqlite3 *db, const char *id, int control) {
    if (sqlite3_get_autocommit(db))
        return -1;
    int slot = control ? -1 : free_slot(db);
    if (slot == -2)
        return -1;
    if (slot < 0) {
        sqlite3_stmt *s = prepare(
            db, "SELECT count(*) FROM jobs WHERE kind=? AND state!='done' AND worker_slot IS NULL");
        if (!s)
            return -1;
        sqlite3_bind_text(s, 1, control ? "control" : "runtime", -1, SQLITE_STATIC);
        int rc = sqlite3_step(s);
        int full = rc == SQLITE_ROW && sqlite3_column_int(s, 0) >= 64;
        sqlite3_finalize(s);
        if (rc != SQLITE_ROW)
            return -1;
        if (full)
            return 1;
    }
    sqlite3_stmt *s = prepare(db, "INSERT INTO jobs VALUES(?,?,?,?)");
    if (!s)
        return -1;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, control ? "control" : "runtime", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, slot >= 0 ? "reserved" : "queued", -1, SQLITE_STATIC);
    if (slot >= 0)
        sqlite3_bind_int(s, 4, slot);
    else
        sqlite3_bind_null(s, 4);
    return finish(s);
}
int gstd_schedule_take(sqlite3 *db, int control, char id[65]) {
    id[0] = 0;
    if (execute(db, "BEGIN IMMEDIATE"))
        return -1;
    sqlite3_stmt *s = prepare(
        db, "SELECT j.operation_id FROM jobs j JOIN operations o ON o.operation_id=j.operation_id "
            "WHERE j.kind=? AND j.state=? AND o.state='accepted' ORDER BY j.rowid LIMIT 1");
    if (!s)
        goto rollback;
    sqlite3_bind_text(s, 1, control ? "control" : "runtime", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, control ? "queued" : "reserved", -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(s);
        if (execute(db, "COMMIT"))
            goto rollback;
        return 0;
    }
    const char *found = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(s, 0) : NULL;
    if (!found) {
        sqlite3_finalize(s);
        goto rollback;
    }
    snprintf(id, 65, "%s", found);
    sqlite3_finalize(s);
    s = prepare(db, "UPDATE jobs SET state='running' WHERE operation_id=?");
    if (!s)
        goto rollback;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    if (finish(s))
        goto rollback;
    s = prepare(db, "UPDATE operations SET state='running' WHERE operation_id=?");
    if (!s)
        goto rollback;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    if (finish(s) || execute(db, "COMMIT"))
        goto rollback;
    return 1;
rollback:
    (void)execute(db, "ROLLBACK");
    return -1;
}
int gstd_schedule_finish(sqlite3 *db, const char *id) {
    if (execute(db, "BEGIN IMMEDIATE"))
        return -1;
    sqlite3_stmt *s =
        prepare(db, "SELECT j.worker_slot,j.state,o.state FROM jobs j JOIN operations o ON "
                    "o.operation_id=j.operation_id WHERE j.operation_id=?");
    if (!s)
        goto rollback;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(s);
        goto rollback;
    }
    const char *state = (const char *)sqlite3_column_text(s, 2);
    int terminal = state && (!strcmp(state, "succeeded") || !strcmp(state, "failed") ||
                             !strcmp(state, "cancelled"));
    int slot = sqlite3_column_type(s, 0) == SQLITE_NULL ? -1 : sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (!terminal)
        goto rollback;
    s = prepare(db, "UPDATE jobs SET state='done',worker_slot=NULL WHERE operation_id=?");
    if (!s)
        goto rollback;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    if (finish(s))
        goto rollback;
    if (slot >= 0) {
        s = prepare(db, "UPDATE jobs SET state='reserved',worker_slot=? WHERE operation_id=(SELECT "
                        "operation_id FROM jobs WHERE kind='runtime' AND state='queued' ORDER BY "
                        "rowid LIMIT 1)");
        if (!s)
            goto rollback;
        sqlite3_bind_int(s, 1, slot);
        if (finish(s))
            goto rollback;
    }
    if (execute(db, "COMMIT"))
        goto rollback;
    return 0;
rollback:
    (void)execute(db, "ROLLBACK");
    return -1;
}
int gstd_schedule_observer_reserve(sqlite3 *db, const char *id) {
    if (!db || !id || !sqlite3_get_autocommit(db) || execute(db, "BEGIN IMMEDIATE"))
        return -1;
    int slot = free_slot(db);
    if (slot < 0) {
        execute(db, "ROLLBACK");
        return slot == -2 ? -1 : 0;
    }
    sqlite3_stmt *s = prepare(
        db,
        "UPDATE jobs SET state='running',worker_slot=? WHERE operation_id=? AND state='done' "
        "AND EXISTS(SELECT 1 FROM operations o JOIN deployments d ON "
        "d.last_operation_id=o.operation_id "
        "WHERE o.operation_id=jobs.operation_id AND o.state='succeeded' AND "
        "o.method='deployment.start' "
        "AND o.caller_uid=? AND d.desired_state='running' AND d.recovery='normal' AND NOT EXISTS("
        "SELECT 1 FROM operations active WHERE active.deployment_id=d.deployment_id AND "
        "active.state IN ('accepted','running')))");
    if (!s) {
        execute(db, "ROLLBACK");
        return -1;
    }
    sqlite3_bind_int(s, 1, slot);
    sqlite3_bind_text(s, 2, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)geteuid());
    int rc = finish(s), changed = sqlite3_changes(db);
    if (rc || execute(db, "COMMIT")) {
        execute(db, "ROLLBACK");
        return -1;
    }
    return changed == 1 ? 1 : 0;
}
#ifdef GITSTORED_TESTING
static const gstd_backend *controlled_backend;
int gstd_test_backend_register(const gstd_backend *backend) {
    if (!backend || !backend->observe || !backend->cancel || controlled_backend)
        return -1;
    controlled_backend = backend;
    return 0;
}
#endif
