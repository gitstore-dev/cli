#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/ownership.h"
#include "gitstored/protocol.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
int gstd_writer_lock(int directory, const char *id) {
    struct stat st;
    if (!id || !gstd_identifier(id, strlen(id)) || fstat(directory, &st) || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0777) != 0700)
        return -1;
    char name[80];
    if (snprintf(name, sizeof(name), "writer-%s.lock", id) < 0)
        return -1;
    int fd = openat(directory, name, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() ||
        (st.st_mode & 0777) != 0600 || flock(fd, LOCK_EX | LOCK_NB)) {
        close(fd);
        return -1;
    }
    return fd;
}
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
static int mark(sqlite3 *db, const char *resource, const char *deployment, const char *operation,
                const char *ownership, const char *recovery) {
    if (execute(db, "BEGIN IMMEDIATE"))
        return -1;
    sqlite3_stmt *s = prepare(
        db,
        "UPDATE resource_ownership SET ownership_state=?,lifecycle_stage=CASE WHEN ?='released' "
        "THEN 'retained' ELSE lifecycle_stage END,observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') "
        "WHERE resource_id=? AND deployment_id=? AND operation_id=?");
    if (!s)
        goto rollback;
    sqlite3_bind_text(s, 1, ownership, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, ownership, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, resource, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, deployment, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, operation, -1, SQLITE_TRANSIENT);
    if (finish(s) || sqlite3_changes(db) != 1)
        goto rollback;
    s = prepare(db,
                "UPDATE deployments SET recovery=?,writer_ownership=CASE WHEN EXISTS(SELECT 1 FROM "
                "resource_ownership WHERE deployment_id=? AND resource_type='repository-volume' "
                "AND ownership_state='uncertain') THEN 'uncertain' WHEN EXISTS(SELECT 1 FROM "
                "resource_ownership WHERE deployment_id=? AND resource_type='repository-volume' "
                "AND ownership_state='held') THEN 'held' ELSE 'released' "
                "END,readiness='pending',observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE "
                "deployment_id=?");
    if (!s)
        goto rollback;
    sqlite3_bind_text(s, 1, recovery, -1, SQLITE_STATIC);
    for (int i = 2; i <= 4; ++i)
        sqlite3_bind_text(s, i, deployment, -1, SQLITE_TRANSIENT);
    if (finish(s) || sqlite3_changes(db) != 1)
        goto rollback;
    s = prepare(db,
                "UPDATE operations SET recovery=?,updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') "
                "WHERE operation_id=?");
    if (!s)
        goto rollback;
    sqlite3_bind_text(s, 1, recovery, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, operation, -1, SQLITE_TRANSIENT);
    if (finish(s) || sqlite3_changes(db) != 1 || execute(db, "COMMIT"))
        goto rollback;
    return 0;
rollback:
    (void)execute(db, "ROLLBACK");
    return -1;
}
int gstd_ownership_reconcile(sqlite3 *db, const char *resource, const char *deployment,
                             gstd_owner_observe observer) {
    if (!resource || !deployment || !observer || !sqlite3_get_autocommit(db))
        return -1;
    sqlite3_stmt *s =
        prepare(db, "SELECT backend_identity,operation_id FROM resource_ownership WHERE "
                    "resource_id=? AND deployment_id=? AND resource_type='repository-volume'");
    if (!s)
        return -1;
    sqlite3_bind_text(s, 1, resource, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, deployment, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return -1;
    }
    const char *value = (const char *)sqlite3_column_text(s, 0);
    char *backend = value ? strdup(value) : NULL;
    value = (const char *)sqlite3_column_text(s, 1);
    char *operation = value ? strdup(value) : NULL;
    sqlite3_finalize(s);
    if (!operation) {
        free(backend);
        return -1;
    }
    int result = -1;
    if (mark(db, resource, deployment, operation, "uncertain", "reconciling"))
        goto done;
    int terminated = -1;
    /* The observer must check the actual backend, including absent recorded IDs.
     * No open transaction/second writer or arbitrary host execution is involved. */
    int observed = observer(resource, backend, &terminated);
    if (observed || terminated < 0 || terminated > 1) {
        (void)mark(db, resource, deployment, operation, "uncertain", "uncertain");
        goto done;
    }
    if (mark(db, resource, deployment, operation, terminated ? "released" : "held", "normal"))
        goto done;
    result = terminated ? 1 : 0;
done:
    free(backend);
    free(operation);
    return result;
}
