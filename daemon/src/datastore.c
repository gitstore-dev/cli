#include "datastore.h"
#include "gitstored/assets.h"
#include "native_exit.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
enum { ALLOCATING = 1, SCOPE, ASSET, MATERIALIZING, FINISHED };
static int finish(gstd_datastore *d, int outcome, int detail) {
    d->stage = FINISHED;
    d->outcome = outcome;
    d->detail = detail;
    return 1;
}
static int commit(sqlite3 *db, int failed) {
    if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}
static gstd_filesystem_input input(gstd_datastore *d, gstd_filesystem_action action,
                                   int destination, int source) {
    return (gstd_filesystem_input){.action = action,
                                   .manifest = d->target->manifest,
                                   .store_id = d->target->store_id,
                                   .operation_id = d->target->operation_id,
                                   .deployment_id = d->target->deployment_id,
                                   .generation = d->target->generation,
                                   .descriptors = {destination, source, -1},
                                   .download_limit = UINT64_C(512) * 1024 * 1024,
                                   .merge_limit = UINT64_C(1024) * 1024 * 1024,
                                   .image_timeout_ms = 180000};
}
static int scope_launch(gstd_datastore *d) {
    sqlite3 *db = d->environment->database;
    gstd_deployment_workload *w = &d->workloads[d->index];
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return finish(d, -2, 0);
    if (commit(db, gstd_deployment_resource_reserve(db, d->target, w->resource, "workload")))
        return finish(d, -2, 0);
    gstd_filesystem_input request = input(d, GSTD_FS_SCOPE, d->environment->resource_fd, -1);
    request.resource_id = w->resource;
    request.resource_kind = "workload";
    d->stage = SCOPE;
    if (gstd_filesystem_launch(&request, 10000, &d->filesystem))
        return finish(d, -1, 0);
    return 0;
}
int gstd_datastore_prepare_begin(const gstd_deployment_environment *environment,
                                 const gstd_deployment_target *target, const char *configuration,
                                 gstd_datastore *d) {
    const char *store =
        target ? json_string_value(json_object_get(target->manifest, "datastore")) : NULL;
    if (!environment || !environment->database || !environment->settings || !target || !d ||
        d->stage || !store || strcmp(store, "scylladb") || !configuration ||
        strlen(configuration) >= sizeof(d->configuration_id) || environment->cache_fd < 3 ||
        environment->resource_fd < 3 || !sqlite3_get_autocommit(environment->database) ||
        gstd_asset_manifest_check(target->manifest))
        return -1;
    *d = (gstd_datastore){
        .environment = environment, .target = target, .volume = {.data_fd = -1, .writer_fd = -1}};
    memcpy(d->configuration_id, configuration, strlen(configuration) + 1);
    const char *roles[] = {"scylladb", "cql", "schema"};
    for (unsigned i = 0; i < 3; i++) {
        gstd_deployment_workload *w = &d->workloads[i];
        w->name = i == 2 ? "api" : "scylladb";
        w->rootfs_fd = w->death_fd = -1;
        w->population = (gstd_native_population)GSTD_NATIVE_POPULATION_INITIAL;
        int n =
            snprintf(w->resource, sizeof(w->resource), "run-%s-%s", roles[i], target->operation_id);
        int m = snprintf(w->asset_resource, sizeof(w->asset_resource), "asset-%s-%s", roles[i],
                         target->operation_id);
        if (n < 1 || n >= (int)sizeof(w->resource) || m < 1 || m >= (int)sizeof(w->asset_resource))
            return -1;
    }
    int failed =
        target->migration
            ? gstd_migration_datastore_begin(environment->database, &environment->volume_root,
                                             target->operation_id, &d->migration_allocation)
            : gstd_datastore_start_begin(environment->database, target, &environment->volume_root,
                                         configuration, &d->allocation);
    if (failed)
        return -1;
    d->stage = ALLOCATING;
    return 0;
}
int gstd_datastore_prepare_poll(gstd_datastore *d, int cancellation) {
    if (!d || !d->stage || !sqlite3_get_autocommit(d->environment->database))
        return -1;
    if (d->stage == FINISHED)
        return 1;
    if (d->stage == ALLOCATING && d->target->migration) {
        gstd_migration_datastore *a = &d->migration_allocation;
        int ready = gstd_migration_datastore_poll(a, cancellation);
        if (ready != 1)
            return ready;
        if (a->outcome)
            return finish(d, a->outcome, a->detail);
        d->volume = a->volume;
        a->volume.data_fd = a->volume.writer_fd = -1;
        memcpy(d->volume_id, a->volume_id, sizeof(d->volume_id));
        memcpy(d->producer_id, a->producer_id, sizeof(d->producer_id));
        if (gstd_migration_datastore_close(a))
            return finish(d, -2, 0);
        return scope_launch(d);
    }
    if (d->stage == ALLOCATING) {
        int ready = gstd_datastore_start_poll(&d->allocation, cancellation);
        if (ready != 1)
            return ready;
        if (d->allocation.outcome)
            return finish(d, d->allocation.outcome, d->allocation.detail);
        d->graph = d->allocation.graph;
        d->volume = d->allocation.volume;
        d->allocation.volume.data_fd = d->allocation.volume.writer_fd = -1;
        memcpy(d->volume_id, d->allocation.volume_id, sizeof(d->volume_id));
        memcpy(d->producer_id, d->allocation.producer_id, sizeof(d->producer_id));
        if (gstd_datastore_start_close(&d->allocation))
            return finish(d, -2, 0);
        return scope_launch(d);
    }
    gstd_deployment_workload *w = &d->workloads[d->index];
    if (d->stage == ASSET) {
        int ready = gstd_deployment_asset_poll(&d->asset, cancellation);
        if (ready != 1)
            return ready;
        if (d->asset.outcome)
            return finish(d, d->asset.outcome, d->asset.detail);
        gstd_filesystem_input request =
            input(d, GSTD_FS_IMAGE_MATERIALIZE, w->scope.descriptors[1], d->asset.active_fd);
        request.component = w->name;
        d->stage = MATERIALIZING;
        if (gstd_filesystem_launch(&request, 190000, &d->filesystem))
            return finish(d, -1, 0);
        return 0;
    }
    gstd_filesystem_reply reply = {0};
    int ready = gstd_filesystem_poll(&d->filesystem, cancellation, &reply);
    if (ready != 1)
        return ready;
    if (reply.status) {
        int status = reply.status, detail = reply.detail;
        gstd_filesystem_reply_close(&reply);
        return finish(d, status == -2 ? -2 : -1, detail);
    }
    sqlite3 *db = d->environment->database;
    if (d->stage == SCOPE) {
        w->scope = reply;
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            return finish(d, -2, 0);
        int failed =
            gstd_deployment_scope_record(db, d->target, w->resource, &reply) ||
            gstd_deployment_resource_reserve(db, d->target, w->asset_resource, "asset-root");
        if (commit(db, failed))
            return finish(d, -2, 0);
        d->stage = ASSET;
        if (gstd_deployment_asset_begin(
                db, d->target, w->name, w->asset_resource, d->environment->cache_fd,
                gstd_runtime_settings_image(d->environment->settings, w->name),
                UINT64_C(512) * 1024 * 1024, UINT64_C(1024) * 1024 * 1024,
                UINT64_C(16) * 1024 * 1024 * 1024, &d->asset))
            return finish(d, -1, 0);
        return 0;
    }
    if (d->stage != MATERIALIZING || reply.descriptor_count != 1) {
        gstd_filesystem_reply_close(&reply);
        return finish(d, -2, 0);
    }
    w->rootfs_fd = reply.descriptors[0];
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return finish(d, -2, 0);
    if (commit(db, gstd_deployment_execution_record(db, d->target, w->resource, &w->scope,
                                                    w->rootfs_fd)) ||
        gstd_deployment_asset_close(&d->asset))
        return finish(d, -2, 0);
    return ++d->index < 3 ? scope_launch(d) : finish(d, 0, 0);
}
int gstd_datastore_observers_close(gstd_datastore *d) {
    if (!d || d->stage != FINISHED || d->filesystem.worker || d->asset.worker.worker ||
        d->allocation.worker.worker || d->migration_allocation.worker.worker ||
        d->service_job.worker || d->helper.helper.worker.worker)
        return -1;
    if (d->helper.stage && gstd_helper_owner_observers_close(&d->helper))
        return -1;
    for (unsigned i = 0; i < 3; i++) {
        gstd_deployment_workload *w = &d->workloads[i];
        if (w->death_fd >= 3)
            close(w->death_fd);
        gstd_native_population_close(&w->population);
        json_decref((json_t *)w->native.configuration);
        if (w->rootfs_fd >= 3)
            close(w->rootfs_fd);
        gstd_filesystem_reply_close(&w->scope);
    }
    if (d->asset.stage)
        (void)gstd_deployment_asset_close(&d->asset);
    if (d->allocation.stage)
        (void)gstd_datastore_start_close(&d->allocation);
    if (d->migration_allocation.stage)
        (void)gstd_migration_datastore_close(&d->migration_allocation);
    gstd_volume_handle_close(&d->volume);
    memset(d, 0, sizeof(*d));
    return 0;
}
int gstd_datastore_recovery_seed(const gstd_deployment_environment *environment,
                                 const gstd_deployment_target *target, const char *configuration,
                                 gstd_datastore *d) {
    if (!d || d->stage || !environment || !target || !configuration || !*configuration ||
        strlen(configuration) >= sizeof(d->configuration_id) ||
        gstd_asset_manifest_check(target->manifest))
        return -1;
    const char *store = json_string_value(json_object_get(target->manifest, "datastore"));
    if (!store || strcmp(store, "scylladb"))
        return -1;
    *d = (gstd_datastore){.environment = environment,
                          .target = target,
                          .stage = FINISHED,
                          .index = 3,
                          .volume = {.data_fd = -1, .writer_fd = -1}};
    memcpy(d->configuration_id, configuration, strlen(configuration) + 1);
    const char *roles[] = {"scylladb", "cql", "schema"};
    for (unsigned i = 0; i < 3; i++) {
        gstd_deployment_workload *w = &d->workloads[i];
        w->name = i == 2 ? "api" : "scylladb";
        w->rootfs_fd = w->death_fd = -1;
        w->population = (gstd_native_population)GSTD_NATIVE_POPULATION_INITIAL;
        int n =
            snprintf(w->resource, sizeof(w->resource), "run-%s-%s", roles[i], target->operation_id);
        int m = snprintf(w->asset_resource, sizeof(w->asset_resource), "asset-%s-%s", roles[i],
                         target->operation_id);
        if (n < 1 || n >= (int)sizeof(w->resource) || m < 1 || m >= (int)sizeof(w->asset_resource))
            return -1;
        for (unsigned j = 0; j < GSTD_MOUNT_COUNT; j++)
            w->inputs.role_fds[j] = -1;
    }
    return 0;
}
int gstd_datastore_helper_begin(gstd_datastore *d, gstd_execution_purpose purpose,
                                const gstd_network_namespace *network, const char *boot,
                                int resolver, int output, int error) {
    if (!d || d->stage != FINISHED || d->outcome || d->index != 3 || !d->environment ||
        !d->environment->database || !d->target || d->helper.stage ||
        purpose < GSTD_EXEC_SCYLLA_READY || purpose > GSTD_EXEC_SCHEMA_HISTORY ||
        (!d->target->migration && purpose > GSTD_EXEC_SCHEMA_MIGRATE) ||
        (d->target->migration && purpose == GSTD_EXEC_SCYLLA_KEYSPACE) || !network || !boot ||
        strlen(boot) != 36 || strcmp(boot, d->service_boot) || resolver < 3 ||
        (output != -1 && output < 3) || (error != -1 && error < 3) ||
        !sqlite3_get_autocommit(d->environment->database) || gstd_datastore_service_live(d))
        return -1;
    unsigned index = purpose == GSTD_EXEC_SCHEMA_MIGRATE ? 2 : 1;
    gstd_deployment_workload *w = &d->workloads[index];
    if (w->native.configuration || w->rootfs_fd < 3 || w->scope.descriptor_count != 3)
        return -1;
    w->inputs = (gstd_execution_inputs){.manifest = d->target->manifest,
                                        .component = w->name,
                                        .purpose = purpose,
                                        .rootfs_fd = w->rootfs_fd,
                                        .network = network};
    for (unsigned i = 0; i < GSTD_MOUNT_COUNT; i++)
        w->inputs.role_fds[i] = -1;
    w->inputs.role_fds[GSTD_MOUNT_RESOLVER] = resolver;
    w->native = (gstd_native_context){.backend_id = w->resource,
                                      .component = w->name,
                                      .boot_id = boot,
                                      .state_fd = d->environment->native_fd,
                                      .bundle_fd = w->scope.descriptors[2],
                                      .rootfs_fd = w->rootfs_fd,
                                      .stdout_fd = output,
                                      .stderr_fd = error,
                                      .writer_lease_fd = -1,
                                      .control_owner = getpid(),
                                      .registered_inputs = &w->inputs};
    w->native.configuration = gstd_execution_build(&w->inputs);
    if (!w->native.configuration ||
        gstd_helper_owner_begin(d->environment->database, d->target, d->configuration_id,
                                &w->native, &d->helper)) {
        json_decref((json_t *)w->native.configuration);
        w->native.configuration = NULL;
        return -1;
    }
    d->helper_index = index;
    return 0;
}
int gstd_datastore_helper_poll(gstd_datastore *d, int cancellation) {
    return d && d->helper.stage ? gstd_helper_owner_poll(&d->helper, cancellation) : -1;
}
int gstd_datastore_helper_close(gstd_datastore *d) {
    if (!d || d->helper_index < 1 || d->helper_index > 2 || gstd_helper_owner_close(&d->helper))
        return -1;
    gstd_deployment_workload *w = &d->workloads[d->helper_index];
    json_decref((json_t *)w->native.configuration);
    memset(&w->native, 0, sizeof(w->native));
    d->helper_index = 0;
    return 0;
}
