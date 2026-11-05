#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "restoration.h"
#include "gitstored/protocol.h"
#include "native_exit.h"
#include "native_recovery.h"
#include "network_restore.h"
#include "termination.h"
#include "volume_records.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
enum { CONFIG = 1, VOLUME, WORKLOAD, NATIVE, NETWORK_SCOPE, NETWORK, DONE, DATASTORE_VOLUME };
static unsigned workload_count(const gstd_deployment *d) {
    return d->count + (d->persistent ? 3u : 0u);
}
static unsigned resource_count(const gstd_deployment *d) {
    return 3 + 2 * d->count + (d->persistent ? 7u : 0u);
}
static gstd_deployment_workload *workload(gstd_restoration *r) {
    gstd_deployment *d = r->deployment;
    return (unsigned)r->index < d->count ? &d->workloads[r->index]
                                         : &d->datastore.workloads[(unsigned)r->index - d->count];
}
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static sqlite3 *database(gstd_restoration *r) { return r->deployment->environment->database; }
static int finish(gstd_restoration *r, int outcome) {
    json_decref(r->expected);
    r->expected = NULL;
    for (unsigned i = 0; i < 6; i++) {
        json_decref(r->discovered[i]);
        r->discovered[i] = NULL;
    }
    r->outcome = outcome;
    r->stage = DONE;
    return 1;
}
static int load(gstd_restoration *r, const char *id, const char *kind) {
    json_decref(r->expected);
    r->expected = NULL;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(
            database(r),
            "SELECT backend_identity,lifecycle_stage FROM resource_ownership WHERE "
            "resource_id=? AND resource_type=? AND operation_id=? AND deployment_id=? "
            "AND ownership_state IN ('held','uncertain')",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, kind, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, r->deployment->target.operation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, r->deployment->target.deployment_id, -1, SQLITE_TRANSIENT);
    int found = sqlite3_step(s) == SQLITE_ROW;
    const char *lifecycle = found ? (const char *)sqlite3_column_text(s, 1) : NULL;
    r->terminated = lifecycle && !strcmp(lifecycle, "retained");
    r->lifecycle[0] = 0;
    if (lifecycle && strlen(lifecycle) < sizeof(r->lifecycle))
        memcpy(r->lifecycle, lifecycle, strlen(lifecycle) + 1);
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
        if (d->persistent) {
            unsigned offset = 3 + 2 * d->count;
            if (!strcmp(id, d->datastore.volume_id) && !strcmp(kind, "datastore-volume"))
                bit = 1u << offset;
            for (unsigned i = 0; i < 3; i++) {
                if (!strcmp(id, d->datastore.workloads[i].resource) && !strcmp(kind, "workload"))
                    bit = 1u << (offset + 1 + 2 * i);
                if (!strcmp(id, d->datastore.workloads[i].asset_resource) &&
                    !strcmp(kind, "asset-root"))
                    bit = 1u << (offset + 2 + 2 * i);
            }
        }
        if (!bit || (seen & bit)) {
            valid = 0;
            break;
        }
        seen |= bit;
    }
    sqlite3_finalize(s);
    return valid && step == SQLITE_DONE && seen == ((1u << resource_count(d)) - 1) ? 0 : -1;
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
                                         d->volume_id, d->producer_id, &expected))
        return -1;
    if (d->persistent && gstd_scylla_volume_record_recovery_read(
                             environment->database, &environment->volume_root, source,
                             d->datastore.volume_id, d->datastore.producer_id, &expected))
        return -1;
    if (inventory(r) || scope(r, d->configuration_id, "configuration", CONFIG))
        return -1;
    return 0;
}
static int commit(gstd_restoration *r) {
    gstd_deployment *d = r->deployment;
    sqlite3 *db = database(r);
    if (d->persistent && gstd_datastore_service_recovered(&d->datastore, d->boot))
        return -1;
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
    for (unsigned i = 0; valid && i < workload_count(d); i++)
        if (r->discovered[i]) {
            gstd_deployment_workload *w =
                i < d->count ? &d->workloads[i] : &d->datastore.workloads[i - d->count];
            valid = !gstd_native_recovery_record(d, w, r->discovered[i]);
        }
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
        valid = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == (int)resource_count(d);
    }
    sqlite3_finalize(s);
    if (!valid || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    d->recovery_proven = 1;
    return 0;
}
static int next_workload(gstd_restoration *r) {
    gstd_deployment *d = r->deployment;
    if (++r->index < (int)workload_count(d)) {
        if (scope(r, workload(r)->resource, "workload", WORKLOAD))
            return finish(r, -2);
    } else if (scope(r, d->network_id, "network", NETWORK_SCOPE))
        return finish(r, -2);
    return 0;
}
int gstd_restoration_poll(gstd_restoration *r) {
    if (!r || !r->deployment || !r->stage)
        return -1;
    if (r->stage == DONE)
        return 1;
    if (r->native.worker && r->native.deadline_ms > r->deadline_ms)
        r->native.deadline_ms = r->deadline_ms;
    if (r->filesystem.worker && r->filesystem.deadline_ms > r->deadline_ms)
        r->filesystem.deadline_ms = r->deadline_ms;
    if (now_ms() >= r->deadline_ms && !r->filesystem.worker && !r->native.worker)
        return finish(r, -2);
    gstd_deployment *d = r->deployment;
    if (r->stage == NATIVE) {
        gstd_native_status status;
        gstd_native_observation actual = {0};
        int ready = gstd_native_poll(&r->native, -1, &status, &actual);
        if (ready < 0)
            return -1;
        if (!ready)
            return 0;
        gstd_deployment_workload *w = workload(r);
        if (r->discovered[r->index]) {
            w->native.expected_pid = actual.pid;
            w->native.expected_start_time = actual.start_time;
        }
        if (status == GSTD_NATIVE_OK && actual.stopped && !r->discovered[r->index] &&
            actual.pid == w->native.expected_pid &&
            actual.start_time == w->native.expected_start_time) {
            json_t *population = json_object_get(r->expected, "population");
            if (!population ||
                gstd_native_population_reopen(population, w->resource, d->boot, &w->population) ||
                gstd_native_population_empty(&w->population) != 1)
                return finish(r, -2);
            w->observed = actual;
            w->launched = 1;
            w->exited = 1;
            w->recovered_stopped = 1;
            return next_workload(r);
        }
        if (status != GSTD_NATIVE_OK || (!actual.running && !actual.created) ||
            actual.pid != w->native.expected_pid ||
            actual.start_time != w->native.expected_start_time ||
            gstd_native_watch_open(actual.pid, actual.start_time, d->boot, &w->death_fd) ||
            gstd_native_population_open(w->resource, actual.pid, actual.start_time, d->boot,
                                        &w->population) ||
            gstd_native_population_empty(&w->population) != 0)
            return finish(r, -2);
        json_t *recorded_population = json_object_get(r->expected, "population");
        if (recorded_population) {
            json_t *current = gstd_native_population_identity(&w->population, w->resource, d->boot);
            int matches = current && json_equal(current, recorded_population);
            json_decref(current);
            if (!matches)
                return finish(r, -2);
        }
        w->observed = actual;
        w->launched = 1;
        return next_workload(r);
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
        if (d->persistent) {
            if (gstd_deployment_graph_recovery_read(&d->target, r->expected, &d->graph))
                return finish(r, -2);
            d->datastore.graph = d->graph;
        }
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
        if (d->persistent) {
            gstd_volume_identity expected;
            if (gstd_scylla_volume_record_recovery_read(
                    database(r), &d->environment->volume_root, d->target.operation_id,
                    d->datastore.volume_id, d->datastore.producer_id, &expected))
                return finish(r, -2);
            gstd_filesystem_input input = {.action = GSTD_FS_OBSERVE_DATASTORE,
                                           .manifest = d->target.manifest,
                                           .volume_root = d->environment->volume_root,
                                           .volume_expected = expected,
                                           .volume_id = d->datastore.volume_id,
                                           .producer_id = d->datastore.producer_id,
                                           .descriptors = {d->environment->volume_root.root_fd,
                                                           d->environment->volume_root.lock_fd,
                                                           -1}};
            if (gstd_filesystem_launch(&input, 5000, &r->filesystem))
                return finish(r, -2);
            r->stage = DATASTORE_VOLUME;
            return 0;
        }
        if (scope(r, d->workloads[0].resource, "workload", WORKLOAD))
            return finish(r, -2);
        return 0;
    }
    if (r->stage == DATASTORE_VOLUME) {
        if (reply.descriptor_count != 1) {
            gstd_filesystem_reply_close(&reply);
            return finish(r, -2);
        }
        d->datastore.volume = (gstd_volume_handle){
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
        gstd_deployment_workload *w = workload(r);
        w->scope = reply;
        w->scope.descriptor_count = 3;
        w->rootfs_fd = reply.descriptors[3];
        if (d->persistent && (unsigned)r->index > d->count) {
            int undispatched = !json_object_get(r->expected, "helperControl") &&
                               !json_object_get(r->expected, "native") &&
                               !json_object_get(r->expected, "population") &&
                               !strcmp(r->lifecycle, "reserved");
            if (!undispatched &&
                gstd_deployment_helper_receipt_read(&d->target, w->resource, r->expected, d->boot,
                                                    d->environment->native_fd))
                return finish(r, -2);
            w->exited = 1;
            return next_workload(r);
        }
        gstd_native_observation recorded;
        int classification =
            gstd_native_recovery_read(r->expected, r->lifecycle, w->resource, w->name, d->boot,
                                      d->target.generation, &recorded);
        if (classification < 0)
            return finish(r, -2);
        if (classification == GSTD_RECOVERY_UNDISPATCHED) {
            /* No native intent was committed, so this owner never dispatched
             * CREATE. Exact scope/execution proof is still required above. */
            w->exited = 1;
            return next_workload(r);
        }
        if (classification == GSTD_RECOVERY_NATIVE_DISCOVER)
            r->discovered[r->index] = json_incref(r->expected);
        w->native = (gstd_native_context){.backend_id = w->resource,
                                          .component = w->name,
                                          .boot_id = d->boot,
                                          .state_fd = d->environment->native_fd,
                                          .bundle_fd = w->scope.descriptors[2],
                                          .rootfs_fd = w->rootfs_fd,
                                          .writer_lease_fd = -1,
                                          .control_owner = getpid(),
                                          .expected_pid = recorded.pid,
                                          .expected_start_time = recorded.start_time};
        if (r->terminated) {
            /* Only a committed post-delete receipt authorizes skipping an old
             * native resource. Missing PID/path or an unproven retained stage
             * cannot substitute for observed termination. */
            if (gstd_termination_read(database(r), &d->target, w->resource, r->expected))
                return finish(r, -2);
            w->exited = 1;
            return next_workload(r);
        }
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
    if (!d || d->finalizing || d->monitor.running || d->filesystem.worker || d->native_job.worker ||
        d->bootstrap.worker || d->asset.worker.worker)
        return -1;
    if (d->persistent && d->datastore.stage && gstd_datastore_observers_close(&d->datastore))
        return -1;
    for (unsigned i = 0; i < 2; i++) {
        if (d->helper_output[i] >= 3)
            close(d->helper_output[i]);
        d->helper_output[i] = -1;
    }
    if (gstd_log_capture_close(&d->capture))
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
    for (unsigned i = 0; i < 6; i++)
        json_decref(r->discovered[i]);
    memset(r, 0, sizeof(*r));
    return 0;
}
