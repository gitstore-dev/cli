#define _POSIX_C_SOURCE 200809L
#include "retention.h"
#include <stdint.h>
#include <string.h>
#include <sys/statvfs.h>
#define MIB (UINT64_C(1024) * 1024)
#define RECORD_LIMIT 100000
#define CONTROL_RECORDS 128
static int scalar(sqlite3 *db, const char *query, sqlite3_int64 *value) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &s, NULL) != SQLITE_OK)
        return -1;
    int ok = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) == SQLITE_INTEGER;
    if (ok)
        *value = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return ok ? 0 : -1;
}
int gstd_retention_collect(sqlite3 *db, unsigned *removed) {
    if (!db || !removed || sqlite3_get_autocommit(db))
        return -1;
    *removed = 0;
    /* Materialized NOT IN sets avoid a quadratic correlated scan of 100k
     * records. Keep every start while its deployment exists, every resource
     * producer (even released), and every dependent operation/observer lease.
     * No resource or credential tombstone is removed by metadata maintenance. */
    const char *query =
        "SELECT o.operation_id FROM operations o WHERE "
        "o.state IN ('succeeded','failed','cancelled') AND o.recovery='normal' AND "
        "unixepoch(o.updated_at)<unixepoch('now')-604800 AND "
        "unixepoch(o.accepted_at)<unixepoch('now')-604800 AND "
        "NOT (o.method='deployment.start' AND o.deployment_id IS NOT NULL AND "
        "o.deployment_id IN (SELECT deployment_id FROM deployments)) AND "
        "o.operation_id NOT IN (SELECT last_operation_id FROM deployments "
        "WHERE last_operation_id IS NOT NULL) AND "
        "o.operation_id NOT IN (SELECT target_operation_id FROM operations "
        "WHERE target_operation_id IS NOT NULL) AND "
        "o.operation_id NOT IN (SELECT operation_id FROM resource_ownership) AND "
        "NOT (o.method IN ('deployment.stop','operation.cancel') AND o.deployment_id IN "
        "(SELECT deployment_id FROM resource_ownership WHERE ownership_state!='released')) AND "
        "o.operation_id NOT IN (SELECT producer_operation_id FROM repository_volumes) AND "
        "o.operation_id NOT IN (SELECT operation_id FROM network_attachments) AND "
        "o.operation_id NOT IN (SELECT producer_operation_id FROM assets) AND "
        "o.operation_id NOT IN (SELECT operation_id FROM assets WHERE operation_id IS NOT NULL) "
        "AND o.operation_id NOT IN (SELECT operation_id FROM jobs WHERE state!='done') AND "
        "o.operation_id NOT IN (SELECT operation_id FROM dedupe WHERE "
        "expires_after>=unixepoch('now')) ORDER BY o.updated_at,o.operation_id LIMIT 256";
    char ids[256][65];
    unsigned count = 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &s, NULL) != SQLITE_OK)
        return -1;
    int step;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(s, 0);
        int bytes = sqlite3_column_bytes(s, 0);
        if (count == 256 || !id || bytes < 1 || bytes > 64 || memchr(id, 0, (size_t)bytes)) {
            sqlite3_finalize(s);
            return -1;
        }
        memcpy(ids[count], id, (size_t)bytes);
        ids[count++][bytes] = 0;
    }
    sqlite3_finalize(s);
    if (step != SQLITE_DONE)
        return -1;
    const char *deletes[] = {"DELETE FROM jobs WHERE operation_id=? AND state='done'",
                             "DELETE FROM dedupe WHERE operation_id=?",
                             "DELETE FROM operations WHERE operation_id=?"};
    for (unsigned i = 0; i < 3; i++) {
        s = NULL;
        if (sqlite3_prepare_v2(db, deletes[i], -1, &s, NULL) != SQLITE_OK)
            return -1;
        for (unsigned j = 0; j < count; j++) {
            sqlite3_reset(s);
            sqlite3_bind_text(s, 1, ids[j], -1, SQLITE_TRANSIENT);
            if (sqlite3_step(s) != SQLITE_DONE || (i == 2 && sqlite3_changes(db) != 1)) {
                sqlite3_finalize(s);
                return -1;
            }
        }
        sqlite3_finalize(s);
    }
    *removed = count;
    return 0;
}
static int space(sqlite3 *db, int control, size_t intent_bytes) {
    sqlite3_int64 pages, free_pages, maximum, page_size, active;
    if (scalar(db, "PRAGMA page_count", &pages) ||
        scalar(db, "PRAGMA freelist_count", &free_pages) ||
        scalar(db, "PRAGMA max_page_count", &maximum) ||
        scalar(db, "PRAGMA page_size", &page_size) ||
        scalar(db, "SELECT count(*) FROM operations WHERE state IN ('accepted','running')",
               &active))
        return -1;
    if (page_size != 4096 || pages < 0 || pages > 32000 || maximum > 32000 || maximum < pages ||
        free_pages < 0 || free_pages > pages || active < 0 || active > 136 || intent_bytes > MIB)
        return 1;
    /* Reserve 512 KiB of generated stage/result metadata per active operation and this
     * acceptance, plus control/health working space. Rollback journals may hold
     * every database page; leave room for their page headers and filesystem work.
     * This is measured headroom, not a promise against unrelated disk writers. */
    uint64_t available = (uint64_t)(maximum - pages + free_pages) * 4096;
    uint64_t reserve =
        (uint64_t)(active + 1) * (MIB / 2) + (control ? MIB : 8 * MIB) + 2 * (uint64_t)intent_bytes;
    if (available < reserve)
        return 1;
    const char *path = sqlite3_db_filename(db, "main");
    struct statvfs fs;
    if (!path || !*path || statvfs(path, &fs) || !fs.f_frsize)
        return -1;
    uint64_t journal = (uint64_t)pages * (4096 + 8) + MIB;
    uint64_t required = reserve + journal;
    return (uint64_t)fs.f_bavail < (required + fs.f_frsize - 1) / fs.f_frsize ? 1 : 0;
}
int gstd_retention_admit(sqlite3 *db, int control, size_t intent_bytes) {
    if (!db || sqlite3_get_autocommit(db))
        return -1;
    sqlite3_int64 count;
    if (scalar(db, "SELECT count(*) FROM operations", &count))
        return -1;
    int enough = space(db, control, intent_bytes);
    if (enough < 0)
        return -1;
    sqlite3_int64 limit = control ? RECORD_LIMIT : RECORD_LIMIT - CONTROL_RECORDS;
    if (count >= limit || enough) {
        unsigned removed;
        if (gstd_retention_collect(db, &removed) ||
            scalar(db, "SELECT count(*) FROM operations", &count))
            return -1;
        enough = space(db, control, intent_bytes);
    }
    return enough < 0 ? -1 : enough || count >= limit;
}
