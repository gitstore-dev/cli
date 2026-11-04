#include "recovery.h"
#include "gitstored/scheduler.h"
#include <string.h>
int gstd_recovery_invalidate(sqlite3 *db) {
    if (!db || !sqlite3_get_autocommit(db))
        return -1;
    const char *sql =
        "BEGIN IMMEDIATE;"
        "UPDATE resource_ownership SET ownership_state='uncertain',"
        "observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE ownership_state='held' AND "
        "(EXISTS(SELECT 1 FROM operations o WHERE o.operation_id=resource_ownership.operation_id "
        "AND o.state='running') OR EXISTS(SELECT 1 FROM deployments d WHERE "
        "d.deployment_id=resource_ownership.deployment_id AND d.desired_state='running' "
        "AND d.readiness='ready') OR EXISTS(SELECT 1 FROM operations o WHERE "
        "o.deployment_id=resource_ownership.deployment_id AND o.state='running') OR EXISTS("
        "SELECT 1 FROM jobs j WHERE j.operation_id=resource_ownership.operation_id AND "
        "j.state='running'));"
        "UPDATE deployments SET readiness='pending',recovery='uncertain',"
        "writer_ownership=CASE WHEN EXISTS(SELECT 1 FROM resource_ownership r WHERE "
        "r.deployment_id=deployments.deployment_id AND r.resource_type='repository-volume' "
        "AND r.ownership_state='uncertain') THEN 'uncertain' ELSE writer_ownership END,"
        "observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE "
        "(readiness='ready' OR EXISTS(SELECT 1 FROM operations o WHERE "
        "o.operation_id=deployments.last_operation_id AND o.state='running') OR EXISTS("
        "SELECT 1 FROM resource_ownership r WHERE r.deployment_id=deployments.deployment_id "
        "AND r.ownership_state='uncertain'));"
        "UPDATE operations SET state='failed',recovery='uncertain',"
        "error='{\"code\":\"OWNERSHIP_UNCERTAIN\",\"stage\":\"recovery\",\"retryable\":false}',"
        "updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE state='running';COMMIT;";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    /* 72 runtime reservations plus at most 64 pending control reservations.
     * An extra final read distinguishes a fully drained maximum from overflow. */
    for (unsigned i = 0; i <= 136; i++) {
        sqlite3_stmt *s = NULL;
        char id[65] = {0};
        if (sqlite3_prepare_v2(
                db,
                "SELECT j.operation_id FROM jobs j JOIN operations o ON "
                "o.operation_id=j.operation_id "
                "WHERE j.state='running' AND o.state IN ('failed','succeeded','cancelled') LIMIT 1",
                -1, &s, NULL) != SQLITE_OK)
            return -1;
        int step = sqlite3_step(s);
        if (step == SQLITE_ROW) {
            const char *value = (const char *)sqlite3_column_text(s, 0);
            if (value && strlen(value) < sizeof(id))
                memcpy(id, value, strlen(value) + 1);
        }
        sqlite3_finalize(s);
        if (step == SQLITE_DONE)
            return 0;
        if (step != SQLITE_ROW || i == 136 || !*id || gstd_schedule_finish(db, id))
            return -1;
    }
    return -1;
}
