#include "datastore_start.h"
#include <string.h>
enum { ALLOCATING = 1, FINISHED };
static int finish(gstd_datastore_start *a, int outcome, int detail) {
    a->stage = FINISHED;
    a->outcome = outcome;
    a->detail = detail;
    return 1;
}
int gstd_datastore_start_begin(sqlite3 *db, const gstd_deployment_target *target,
                               const gstd_volume_root *root, const char *configuration,
                               gstd_datastore_start *a) {
    const char *datastore =
        target ? json_string_value(json_object_get(target->manifest, "datastore")) : NULL;
    if (!db || !sqlite3_get_autocommit(db) || !datastore || strcmp(datastore, "scylladb") ||
        !root || root->root_fd < 3 || root->lock_fd < 3 || !configuration || !a || a->stage)
        return -1;
    *a = (gstd_datastore_start){
        .database = db, .target = target, .root = root, .volume = {.data_fd = -1, .writer_fd = -1}};
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    gstd_accept_status status = gstd_scylla_volume_reserve(db, target->operation_id, a->volume_id);
    gstd_volume_identity expected;
    int existing =
        status == GSTD_ACCEPT_NEW
            ? gstd_scylla_volume_record_begin(db, root, a->volume_id, target->operation_id,
                                              a->producer_id, &expected)
            : -1;
    if (existing < 0 || gstd_deployment_persistent_graph_begin(db, target, configuration,
                                                               a->volume_id, &a->graph)) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        finish(a, -2, 0);
        return 0;
    }
    gstd_filesystem_input input = {.action = GSTD_FS_DATASTORE_VOLUME,
                                   .manifest = target->manifest,
                                   .volume_id = a->volume_id,
                                   .producer_id = a->producer_id,
                                   .volume_root = *root,
                                   .volume_existing = existing,
                                   .volume_expected = expected,
                                   .descriptors = {root->root_fd, root->lock_fd, -1}};
    a->stage = ALLOCATING;
    if (gstd_filesystem_launch(&input, 10000, &a->worker))
        finish(a, -1, 0);
    return 0;
}
int gstd_datastore_start_poll(gstd_datastore_start *a, int cancellation) {
    if (!a || !a->database || !sqlite3_get_autocommit(a->database) || !a->stage)
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
    if (gstd_scylla_volume_record_ready(db, a->root, a->volume_id, a->target->operation_id,
                                        &a->volume) ||
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return finish(a, -2, 0);
    }
    return finish(a, 0, 0);
}
int gstd_datastore_start_close(gstd_datastore_start *a) {
    if (!a || a->stage != FINISHED || a->worker.worker)
        return -1;
    gstd_volume_handle_close(&a->volume);
    memset(a, 0, sizeof(*a));
    return 0;
}
