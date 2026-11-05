#include "migration_datastore.h"
#include <string.h>
enum { ACQUIRING = 1, FINISHED };
static int finish(gstd_migration_datastore *a, int outcome, int detail) {
    a->stage = FINISHED;
    a->outcome = outcome;
    a->detail = detail;
    return 1;
}
int gstd_migration_datastore_begin(sqlite3 *db, const gstd_volume_root *root, const char *operation,
                                   gstd_migration_datastore *a) {
    if (!db || !sqlite3_get_autocommit(db) || !root || root->root_fd < 3 || root->lock_fd < 3 ||
        !a || a->stage || !operation)
        return -1;
    *a = (gstd_migration_datastore){
        .database = db, .root = root, .volume = {.data_fd = -1, .writer_fd = -1}};
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    gstd_volume_identity expected;
    int failed = gstd_migration_execution_read(db, operation, &a->execution);
    gstd_accept_status reserved =
        failed ? GSTD_ACCEPT_INVALID
               : gstd_scylla_migration_volume_reserve(db, operation, a->volume_id);
    failed = failed || (reserved != GSTD_ACCEPT_NEW && reserved != GSTD_ACCEPT_REPLAY) ||
             gstd_scylla_volume_record_begin(db, root, a->volume_id, operation, a->producer_id,
                                             &expected) != 1;
    if (failed) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        gstd_migration_execution_clear(&a->execution);
        memset(a, 0, sizeof(*a));
        return -1;
    }
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        finish(a, -2, 0);
        return 0;
    }
    gstd_filesystem_input input = {.action = GSTD_FS_DATASTORE_VOLUME,
                                   .manifest = a->execution.target.manifest,
                                   .volume_root = *root,
                                   .volume_id = a->volume_id,
                                   .producer_id = a->producer_id,
                                   .volume_existing = 1,
                                   .volume_expected = expected,
                                   .descriptors = {root->root_fd, root->lock_fd, -1}};
    a->stage = ACQUIRING;
    if (gstd_filesystem_launch(&input, 10000, &a->worker))
        finish(a, -1, 0);
    return 0;
}
int gstd_migration_datastore_poll(gstd_migration_datastore *a, int cancellation) {
    if (!a || !a->stage || !a->database || !sqlite3_get_autocommit(a->database))
        return -1;
    if (a->stage == FINISHED)
        return 1;
    gstd_filesystem_reply reply = {0};
    int ready = gstd_filesystem_poll(&a->worker, cancellation, &reply);
    if (ready != 1)
        return ready;
    if (reply.status || reply.descriptor_count != 2) {
        int status = reply.status, detail = reply.detail;
        gstd_filesystem_reply_close(&reply);
        return finish(a, status == -2 ? -2 : -1, detail);
    }
    a->volume = (gstd_volume_handle){.data_fd = reply.descriptors[0],
                                     .writer_fd = reply.descriptors[1],
                                     .identity = reply.volume};
    sqlite3 *db = a->database;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return finish(a, -2, 0);
    if (gstd_scylla_volume_record_ready(db, a->root, a->volume_id, a->execution.target.operation_id,
                                        &a->volume) ||
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return finish(a, -2, 0);
    }
    return finish(a, 0, 0);
}
int gstd_migration_datastore_close(gstd_migration_datastore *a) {
    if (!a || a->stage != FINISHED || a->worker.worker)
        return -1;
    gstd_volume_handle_close(&a->volume);
    gstd_migration_execution_clear(&a->execution);
    memset(a, 0, sizeof(*a));
    return 0;
}
