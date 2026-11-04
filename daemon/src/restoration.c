#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "restoration.h"
#include "gitstored/protocol.h"
#include "native_exit.h"
#include "network_restore.h"
#include "volume_records.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
enum { CONFIG = 1, VOLUME, WORKLOAD, NATIVE, NETWORK_SCOPE, NETWORK, DONE };
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static sqlite3 *database(gstd_restoration *r) { return r->deployment->environment->database; }
static int finish(gstd_restoration *r, int outcome) {
    json_decref(r->expected);
    r->expected = NULL;
    r->outcome = outcome;
    r->stage = DONE;
    return 1;
}
static int text(const json_t *v, const char *field, const char *expected) {
    json_t *value = json_object_get(v, field);
    return expected && json_is_string(value) && json_string_length(value) == strlen(expected) &&
           !memcmp(json_string_value(value), expected, strlen(expected));
}
static int load(gstd_restoration *r, const char *id, const char *kind) {
    json_decref(r->expected);
    r->expected = NULL;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(
            database(r),
            "SELECT backend_identity FROM resource_ownership WHERE "
            "resource_id=? AND resource_type=? AND operation_id=? AND deployment_id=? "
            "AND ownership_state IN ('held','uncertain')",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, kind, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, r->deployment->target.operation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, r->deployment->target.deployment_id, -1, SQLITE_TRANSIENT);
    int found = sqlite3_step(s) == SQLITE_ROW;
    const unsigned char *bytes = found ? sqlite3_column_text(s, 0) : NULL;
    int length = found ? sqlite3_column_bytes(s, 0) : 0;
    if (bytes && length > 0 && length <= 32768)
        r->expected = gstd_json_parse(bytes, (size_t)length);
    sqlite3_finalize(s);
    return r->expected ? 0 : -1;
}
static int scope(gstd_restoration *r, const char *id, const char *kind, int stage) {
    gstd_deployment *d = r->deployment;
    if (load(r, id, kind))
        return -1;
    gstd_filesystem_input input = {.action = GSTD_FS_REOPEN_SCOPE,
                                   .manifest = d->target.manifest,
                                   .scope_expected = r->expected,
                                   .store_id = d->target.store_id,
                                   .operation_id = d->target.operation_id,
                                   .deployment_id = d->target.deployment_id,
                                   .generation = d->target.generation,
                                   .resource_id = id,
                                   .resource_kind = kind,
                                   .descriptors = {d->environment->resource_fd, -1, -1}};
    if (gstd_filesystem_launch(&input, 5000, &r->filesystem))
        return -1;
    r->stage = stage;
    return 0;
}
static int inventory(gstd_restoration *r) {
    gstd_deployment *d = r->deployment;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(
            database(r),
            "SELECT resource_id,resource_type FROM resource_ownership "
            "WHERE operation_id=? AND deployment_id=? AND ownership_state IN ('held','uncertain')",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, d->target.operation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, d->target.deployment_id, -1, SQLITE_TRANSIENT);
    unsigned seen = 0;
    int step, valid = 1;
    while ((step = sqlite3_step(s)) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(s, 0),
                   *kind = (const char *)sqlite3_column_text(s, 1);
        unsigned bit = 0;
        if (!id || !kind || !gstd_identifier(id, (size_t)sqlite3_column_bytes(s, 0))) {
            valid = 0;
            break;
        }
        if (!strcmp(id, d->configuration_id) && !strcmp(kind, "configuration"))
            bit = 1;
        if (!strcmp(id, d->network_id) && !strcmp(kind, "network"))
            bit = 2;
        if (!strcmp(id, d->volume_id) && !strcmp(kind, "repository-volume"))
            bit = 4;
        for (unsigned i = 0; i < d->count; i++) {
            if (!strcmp(id, d->workloads[i].resource) && !strcmp(kind, "workload"))
                bit = 1u << (3 + 2 * i);
            if (!strcmp(id, d->workloads[i].asset_resource) && !strcmp(kind, "asset-root"))
                bit = 1u << (4 + 2 * i);
        }
        if (!bit || (seen & bit)) {
            valid = 0;
            break;
        }
        seen |= bit;
    }
    sqlite3_finalize(s);
    return valid && step == SQLITE_DONE && seen == ((1u << (3 + 2 * d->count)) - 1) ? 0 : -1;
}
int gstd_restoration_begin(const gstd_deployment_environment *environment, const char *source,
                           gstd_deployment *d, gstd_restoration *r) {
    if (!r || r->stage || !d || d->stage)
        return -1;
    int64_t now = now_ms();
    if (now < 0)
        return -1;
    *r = (gstd_restoration){.deployment = d, .deadline_ms = now + 30000};
    if (gstd_deployment_recovery_prepare(environment, source, d))
        return -1;
    gstd_volume_identity expected;
    if (gstd_volume_record_recovery_read(environment->database, &environment->volume_root, source,
                                         d->volume_id, d->producer_id, &expected) ||
        inventory(r) || scope(r, d->configuration_id, "configuration", CONFIG))
        return -1;
    return 0;
}
static int commit(gstd_restoration *r) {
    gstd_deployment *d = r->deployment;
    sqlite3 *db = database(r);
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    sqlite3_stmt *s = NULL;
    int valid =
        sqlite3_prepare_v2(db,
                           "SELECT 1 FROM deployments WHERE deployment_id=? AND generation=? "
                           "AND last_operation_id=?",
                           -1, &s, NULL) == SQLITE_OK;
    if (valid) {
        sqlite3_bind_text(s, 1, d->target.deployment_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)d->recovery_generation);
        sqlite3_bind_text(s, 3, d->recovery_operation, -1, SQLITE_TRANSIENT);
        valid = sqlite3_step(s) == SQLITE_ROW;
    }
    sqlite3_finalize(s);
    s = NULL;
    if (valid)
        valid = !inventory(r);
    if (valid)
        valid = sqlite3_prepare_v2(db,
                                   "UPDATE resource_ownership SET ownership_state='held',"
                                   "observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE "
                                   "operation_id=? AND deployment_id=? "
                                   "AND ownership_state IN ('held','uncertain')",
                                   -1, &s, NULL) == SQLITE_OK;
    if (valid) {
        sqlite3_bind_text(s, 1, d->target.operation_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, d->target.deployment_id, -1, SQLITE_TRANSIENT);
        valid = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == (int)(3 + 2 * d->count);
    }
    sqlite3_finalize(s);
    if (!valid || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    d->recovery_proven = 1;
    return 0;
}
int gstd_restoration_poll(gstd_restoration *r) {
    if (!r || !r->deployment || !r->stage)
        return -1;
    if (r->stage == DONE)
        return 1;
    if (now_ms() >= r->deadline_ms && !r->filesystem.worker && !r->native.worker)
        return finish(r, -2);
    gstd_deployment *d = r->deployment;
    if (r->stage == NATIVE) {
        gstd_native_status status;
        gstd_native_observation actual;
        int ready = gstd_native_poll(&r->native, -1, &status, &actual);
        if (ready < 0)
            return -1;
        if (!ready)
            return 0;
        gstd_deployment_workload *w = &d->workloads[r->index];
        if (status != GSTD_NATIVE_OK || !actual.running || actual.pid != w->native.expected_pid ||
            actual.start_time != w->native.expected_start_time ||
            gstd_native_watch_open(actual.pid, actual.start_time, d->boot, &w->death_fd) ||
            gstd_native_population_open(w->resource, actual.pid, actual.start_time, d->boot,
                                        &w->population) ||
            gstd_native_population_empty(&w->population) != 0)
            return finish(r, -2);
        w->observed = actual;
        w->launched = 1;
        if (++r->index < (int)d->count) {
            if (scope(r, d->workloads[r->index].resource, "workload", WORKLOAD))
                return finish(r, -2);
        } else if (scope(r, d->network_id, "network", NETWORK_SCOPE))
            return finish(r, -2);
        return 0;
    }
    gstd_filesystem_reply reply = {0};
    int ready = gstd_filesystem_poll(&r->filesystem, -1, &reply);
    if (ready < 0)
        return -1;
    if (!ready)
        return 0;
    if (reply.status) {
        gstd_filesystem_reply_close(&reply);
        return finish(r, -2);
    }
    if (r->stage == CONFIG) {
        if (reply.descriptor_count != 2) {
            gstd_filesystem_reply_close(&reply);
            return finish(r, -2);
        }
        d->configuration = reply;
        gstd_volume_identity expected;
        if (gstd_volume_record_recovery_read(database(r), &d->environment->volume_root,
                                             d->target.operation_id, d->volume_id, d->producer_id,
                                             &expected))
            return finish(r, -2);
        gstd_filesystem_input input = {.action = GSTD_FS_OBSERVE_VOLUME,
                                       .volume_root = d->environment->volume_root,
                                       .volume_expected = expected,
                                       .volume_id = d->volume_id,
                                       .producer_id = d->producer_id,
                                       .descriptors = {d->environment->volume_root.root_fd,
                                                       d->environment->volume_root.lock_fd, -1}};
        if (gstd_filesystem_launch(&input, 5000, &r->filesystem))
            return finish(r, -2);
        r->stage = VOLUME;
        return 0;
    }
    if (r->stage == VOLUME) {
        if (reply.descriptor_count != 1) {
            gstd_filesystem_reply_close(&reply);
            return finish(r, -2);
        }
        d->volume = (gstd_volume_handle){
            .data_fd = reply.descriptors[0], .writer_fd = -1, .identity = reply.volume};
        if (scope(r, d->workloads[0].resource, "workload", WORKLOAD))
            return finish(r, -2);
        return 0;
    }
    if (r->stage == WORKLOAD) {
        if (reply.descriptor_count != 4) {
            gstd_filesystem_reply_close(&reply);
            return finish(r, -2);
        }
        gstd_deployment_workload *w = &d->workloads[r->index];
        w->scope = reply;
        w->scope.descriptor_count = 3;
        w->rootfs_fd = reply.descriptors[3];
        json_t *native = json_object_get(r->expected, "native");
        json_int_t pid = json_integer_value(json_object_get(native, "pid")),
                   start = json_integer_value(json_object_get(native, "startTime"));
        if (!json_is_object(native) || json_object_size(native) != 7 ||
            !text(native, "kind", "libcrun") || !text(native, "id", w->resource) ||
            !text(native, "component", w->name) || !text(native, "bootId", d->boot) ||
            !json_is_integer(json_object_get(native, "pid")) || pid <= 0 || pid > INT_MAX ||
            !json_is_integer(json_object_get(native, "startTime")) || start <= 0 ||
            !json_is_integer(json_object_get(native, "generation")) ||
            json_integer_value(json_object_get(native, "generation")) !=
                (json_int_t)d->target.generation)
            return finish(r, -2);
        w->native = (gstd_native_context){.backend_id = w->resource,
                                          .component = w->name,
                                          .boot_id = d->boot,
                                          .state_fd = d->environment->native_fd,
                                          .bundle_fd = w->scope.descriptors[2],
                                          .rootfs_fd = w->rootfs_fd,
                                          .writer_lease_fd = -1,
                                          .control_owner = getpid(),
                                          .expected_pid = (pid_t)pid,
                                          .expected_start_time = (uint64_t)start};
        if (gstd_native_launch(&w->native, GSTD_NATIVE_OBSERVE, 10000, &r->native))
            return finish(r, -2);
        r->stage = NATIVE;
        return 0;
    }
    if (r->stage == NETWORK_SCOPE) {
        if (reply.descriptor_count != 1) {
            gstd_filesystem_reply_close(&reply);
            return finish(r, -2);
        }
        d->network_scope = reply;
        gstd_filesystem_input input = {.action = GSTD_FS_RECOVER_NETWORK,
                                       .manifest = d->target.manifest,
                                       .store_id = d->target.store_id,
                                       .operation_id = d->target.operation_id,
                                       .resource_id = d->network_id,
                                       .scope_expected = r->expected,
                                       .descriptors = {reply.descriptors[0], -1, -1}};
        if (gstd_filesystem_launch(&input, 5000, &r->filesystem))
            return finish(r, -2);
        r->stage = NETWORK;
        return 0;
    }
    if (r->stage == NETWORK) {
        if (gstd_network_restore_adopt(&reply, r->expected, &d->network, &d->network_observed)) {
            gstd_filesystem_reply_close(&reply);
            return finish(r, -2);
        }
        if (gstd_native_watch_open(d->network_observed.network.keeper_pid,
                                   d->network_observed.network.keeper_start,
                                   d->network_observed.network.boot_id, &d->keeper_death_fd) ||
            commit(r))
            return finish(r, -2);
        return finish(r, 0);
    }
    gstd_filesystem_reply_close(&reply);
    return finish(r, -2);
}
int gstd_deployment_observers_close(gstd_deployment *d) {
    if (!d || d->monitor.running || d->filesystem.worker || d->native_job.worker ||
        d->bootstrap.worker || d->asset.worker.worker)
        return -1;
    for (unsigned i = 0; i < d->count && i < 3; i++) {
        gstd_deployment_workload *w = &d->workloads[i];
        for (unsigned j = 0; j < GSTD_MOUNT_COUNT; j++)
            if (j != GSTD_MOUNT_REPOSITORIES && j != GSTD_MOUNT_RESOLVER &&
                w->inputs.role_fds[j] >= 3)
                close(w->inputs.role_fds[j]);
        json_decref((json_t *)w->native.configuration);
        if (w->death_fd >= 3)
            close(w->death_fd);
        if (w->rootfs_fd >= 3)
            close(w->rootfs_fd);
        gstd_native_population_close(&w->population);
        gstd_filesystem_reply_close(&w->scope);
    }
    if (d->asset.stage) {
        if (d->asset.staging_fd >= 3)
            close(d->asset.staging_fd);
        if (d->asset.active_fd >= 3)
            close(d->asset.active_fd);
    }
    if (d->keeper_death_fd >= 3)
        close(d->keeper_death_fd);
    if (d->resolver_fd >= 3)
        close(d->resolver_fd);
    gstd_network_worker_observation_close(&d->network_observed);
    gstd_network_worker_close(&d->network);
    gstd_volume_handle_close(&d->volume);
    gstd_filesystem_reply_close(&d->configuration);
    gstd_filesystem_reply_close(&d->network_scope);
    gstd_deployment_target_clear(&d->target);
    json_decref(d->credential_source);
    memset(d, 0, sizeof(*d));
    return 0;
}
int gstd_restoration_discard(gstd_restoration *r) {
    if (!r || !r->deployment || !r->deployment->recovered || r->filesystem.worker ||
        r->native.worker)
        return -1;
    if (gstd_deployment_observers_close(r->deployment))
        return -1;
    json_decref(r->expected);
    memset(r, 0, sizeof(*r));
    return 0;
}
