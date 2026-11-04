#define _POSIX_C_SOURCE 200809L
#include "deployment_assets.h"
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
enum { WAIT_OWNER = 1, NEW_DIRECTORY, ACQUIRE, PROMOTE, VERIFY, DONE };
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int transaction(sqlite3 *db, int status) {
    if (status || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}
static int fail(gstd_deployment_asset *a, int outcome, int detail) {
    a->stage = DONE;
    a->outcome = outcome;
    a->detail = detail;
    if (a->staging_fd >= 3) {
        close(a->staging_fd);
        a->staging_fd = -1;
    }
    return 1;
}
static int launch(gstd_deployment_asset *a, gstd_filesystem_action action, int target, int source) {
    gstd_filesystem_input input = {.action = action,
                                   .manifest = a->target->manifest,
                                   .component = a->component,
                                   .operation_id = a->target->operation_id,
                                   .asset_key = a->key,
                                   .staging_name = a->staging_name,
                                   .descriptors = {target, source, -1},
                                   .proof = a->proof,
                                   .download_limit = a->download_limit,
                                   .merge_limit = a->merge_limit,
                                   .image_timeout_ms = 180000};
    return gstd_filesystem_launch(&input, 190000, &a->worker);
}
static int reserve(gstd_deployment_asset *a) {
    sqlite3 *db = a->database;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return fail(a, -2, 0);
    gstd_asset_record_status status =
        a->source_fd >= 3
            ? gstd_asset_record_begin_offline(db, a->target->manifest, a->source_fd, a->component,
                                              a->target->operation_id, a->resource,
                                              a->download_limit, a->merge_limit, a->cache_limit,
                                              a->key, a->staging_name)
            : gstd_asset_record_begin(db, a->target->manifest, a->component,
                                      a->target->operation_id, a->resource, a->download_limit,
                                      a->merge_limit, a->cache_limit, a->key, a->staging_name);
    int valid = status == GSTD_ASSET_RECORD_NEW || status == GSTD_ASSET_RECORD_ACTIVE ||
                status == GSTD_ASSET_RECORD_HELD;
    if (transaction(db, !valid))
        return fail(a, valid ? -2 : -1, (int)status);
    if (status == GSTD_ASSET_RECORD_HELD) {
        a->stage = WAIT_OWNER;
        a->retry_ms = now_ms() + 100;
        return 0;
    }
    if (status == GSTD_ASSET_RECORD_NEW) {
        a->stage = NEW_DIRECTORY;
        if (launch(a, GSTD_FS_STAGE_DIRECTORY, a->cache_fd, -1))
            return fail(a, -1, 0);
    } else {
        a->stage = VERIFY;
        if (gstd_asset_record_receipt(db, a->key, a->target->operation_id, a->resource, &a->proof))
            return fail(a, -1, 0);
        if (launch(a, GSTD_FS_OPEN_ACTIVE, a->cache_fd, -1))
            return fail(a, -1, 0);
    }
    return 0;
}
int gstd_deployment_asset_begin(sqlite3 *db, const gstd_deployment_target *target,
                                const char *component, const char *resource, int cache, int source,
                                uint64_t download, uint64_t merge, uint64_t limit,
                                gstd_deployment_asset *a) {
    if (!db || !sqlite3_get_autocommit(db) || !target || !component || !resource || !a ||
        a->stage || a->worker.worker || cache < 3 || (source != -1 && source < 3) ||
        strlen(component) >= sizeof(a->component) || strlen(resource) >= sizeof(a->resource) ||
        !download || !merge || !limit)
        return -1;
    *a = (gstd_deployment_asset){.database = db,
                                 .target = target,
                                 .cache_fd = cache,
                                 .source_fd = source,
                                 .staging_fd = -1,
                                 .active_fd = -1,
                                 .download_limit = download,
                                 .merge_limit = merge,
                                 .cache_limit = limit,
                                 .deadline_ms = now_ms() + 200000};
    memcpy(a->component, component, strlen(component) + 1);
    memcpy(a->resource, resource, strlen(resource) + 1);
    reserve(a);
    return 0;
}
int gstd_deployment_asset_poll(gstd_deployment_asset *a, int cancel) {
    if (!a || !a->database || !a->target || !sqlite3_get_autocommit(a->database) || !a->stage)
        return -1;
    if (a->stage == DONE)
        return 1;
    if (a->stage == WAIT_OWNER) {
        struct pollfd p = {.fd = cancel, .events = POLLIN};
        int64_t now = now_ms();
        if (now < 0 || now >= a->deadline_ms || (cancel >= 0 && poll(&p, 1, 0) > 0 && p.revents))
            return fail(a, -2, 0);
        return now >= a->retry_ms ? reserve(a) : 0;
    }
    gstd_filesystem_reply reply = {0};
    int ready = gstd_filesystem_poll(&a->worker, cancel, &reply);
    if (ready < 0)
        return -1;
    if (!ready)
        return 0;
    if (reply.status) {
        int outcome = reply.status == -2 ? -2 : -1, detail = reply.detail;
        gstd_filesystem_reply_close(&reply);
        return fail(a, outcome, detail);
    }
    sqlite3 *db = a->database;
    if (a->stage == NEW_DIRECTORY) {
        if (reply.descriptor_count != 1) {
            gstd_filesystem_reply_close(&reply);
            return fail(a, -2, 0);
        }
        a->staging_fd = reply.descriptors[0];
        reply.descriptor_count = 0;
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            return fail(a, -2, 0);
        if (transaction(
                db, gstd_asset_record_identity(db, a->key, a->target->operation_id, a->staging_fd)))
            return fail(a, -2, 0);
        if (gstd_asset_record_source(db, a->key, a->target->operation_id, a->source_fd))
            return fail(a, -1, 0);
        a->stage = ACQUIRE;
        if (launch(a, a->source_fd >= 3 ? GSTD_FS_IMAGE_IMPORT : GSTD_FS_IMAGE_STAGE, a->staging_fd,
                   a->source_fd))
            return fail(a, -1, 0);
    } else if (a->stage == ACQUIRE) {
        a->proof = reply.proof;
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            return fail(a, -2, 0);
        if (transaction(db,
                        gstd_asset_record_verified(db, a->key, a->target->operation_id, &a->proof)))
            return fail(a, -2, 0);
        a->stage = PROMOTE;
        if (launch(a, GSTD_FS_IMAGE_PROMOTE, a->cache_fd, -1))
            return fail(a, -1, 0);
    } else if (a->stage == PROMOTE) {
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            return fail(a, -2, 0);
        if (transaction(db,
                        gstd_asset_record_active(db, a->key, a->target->operation_id, a->cache_fd)))
            return fail(a, -2, 0);
        if (a->staging_fd >= 3) {
            close(a->staging_fd);
            a->staging_fd = -1;
        }
        a->stage = VERIFY;
        if (launch(a, GSTD_FS_OPEN_ACTIVE, a->cache_fd, -1))
            return fail(a, -1, 0);
    } else if (a->stage == VERIFY) {
        if (reply.descriptor_count != 1) {
            gstd_filesystem_reply_close(&reply);
            return fail(a, -2, 0);
        }
        a->active_fd = reply.descriptors[0];
        reply.descriptor_count = 0;
        a->stage = DONE;
        a->outcome = 0;
        return 1;
    } else {
        gstd_filesystem_reply_close(&reply);
        return fail(a, -2, 0);
    }
    gstd_filesystem_reply_close(&reply);
    return 0;
}
int gstd_deployment_asset_close(gstd_deployment_asset *a) {
    if (!a || a->stage != DONE || a->worker.worker)
        return -1;
    if (a->staging_fd >= 3)
        close(a->staging_fd);
    if (a->active_fd >= 3)
        close(a->active_fd);
    memset(a, 0, sizeof(*a));
    return 0;
}
