#include "cancellation.h"
#include "gitstored/protocol.h"
#include "gitstored/scheduler.h"
#include <string.h>
static int identity(sqlite3_stmt *s, char output[65]) {
    const char *id = (const char *)sqlite3_column_text(s, 0);
    int bytes = sqlite3_column_bytes(s, 0);
    if (!id || !gstd_identifier(id, (size_t)bytes))
        return -1;
    memcpy(output, id, (size_t)bytes);
    output[bytes] = 0;
    return 0;
}
int gstd_cancellation_next(sqlite3 *db, const char *const *waiting, size_t count,
                           char operation[65]) {
    if (!db || !operation || count > 32 || (count && !waiting) || !sqlite3_get_autocommit(db))
        return -1;
    operation[0] = 0;
    for (size_t i = 0; i < count; i++)
        if (!waiting[i])
            return -1;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(
            db,
            "SELECT j.operation_id FROM jobs j JOIN operations o ON o.operation_id=j.operation_id "
            "WHERE (j.kind='control' AND j.state!='done' AND "
            "o.state IN ('succeeded','failed','cancelled')) OR "
            "(j.kind='runtime' AND j.state IN ('queued','reserved') AND o.state='cancelled') "
            "ORDER BY j.rowid LIMIT 1",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    int step = sqlite3_step(s),
        valid = step == SQLITE_DONE || (step == SQLITE_ROW && !identity(s, operation));
    sqlite3_finalize(s);
    if (!valid || (step == SQLITE_ROW && gstd_schedule_finish(db, operation)))
        return -1;
    operation[0] = 0;
    if (sqlite3_prepare_v2(
            db,
            "SELECT j.operation_id FROM jobs j JOIN operations o ON o.operation_id=j.operation_id "
            "WHERE j.kind='control' AND j.state='running' AND o.state='running' ORDER BY j.rowid "
            "LIMIT 64",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        char id[65];
        if (identity(s, id)) {
            sqlite3_finalize(s);
            return -1;
        }
        size_t i = 0;
        while (i < count && strcmp(id, waiting[i]))
            i++;
        if (i == count) {
            memcpy(operation, id, strlen(id) + 1);
            sqlite3_finalize(s);
            return 1;
        }
    }
    sqlite3_finalize(s);
    if (step != SQLITE_DONE)
        return -1;
    return gstd_schedule_take(db, 1, operation);
}

int gstd_cancellation_can_notify(const gstd_deployment *d) {
    if (!d || !d->count || d->count > 3)
        return 0;
    if (d->graph.persistent && (d->graph.stage == GSTD_BOOT_PREPARE_KEYSPACE ||
                                d->graph.stage == GSTD_BOOT_MIGRATE_SCHEMA))
        return 0;
    return !(d->bootstrap.worker && d->graph.stage == GSTD_BOOT_ENROLL) &&
           !(d->native_job.worker &&
             (d->index >= d->count || !d->workloads[d->index].native.expected_pid));
}
