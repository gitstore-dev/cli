#include "datastore.h"
#include "native_exit.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
enum { CREATING = 1, STARTING, OBSERVING, FINISHED };
static int commit(sqlite3 *db, int failed) {
    if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}
static int finish(gstd_datastore *d, int outcome) {
    d->service_stage = FINISHED;
    d->service_outcome = outcome;
    return 1;
}
int gstd_datastore_service_live(const gstd_datastore *d) {
    if (!d || d->service_stage != FINISHED || d->service_outcome)
        return -1;
    const gstd_deployment_workload *w = &d->workloads[0];
    return w->launched && gstd_native_watch_poll(w->death_fd) == 0 &&
                   !gstd_native_population_matches(&w->population, w->resource,
                                                   w->native.expected_pid,
                                                   w->native.expected_start_time, d->service_boot)
               ? 0
               : -1;
}
int gstd_datastore_service_retire(gstd_datastore *d) {
    if (!d || d->service_stage != FINISHED || d->service_job.worker || d->helper.stage)
        return -1;
    gstd_deployment_workload *w = &d->workloads[0];
    if (w->launched || !w->exited ||
        (!(w->recovered_stopped && w->observed.stopped && w->death_fd == -1) &&
         gstd_native_watch_poll(w->death_fd) != 1) ||
        gstd_native_population_empty(&w->population) != 1)
        return -1;
    json_decref((json_t *)w->native.configuration);
    w->native.configuration = NULL;
    d->service_stage = 0;
    return 0;
}
int gstd_datastore_service_recovered(gstd_datastore *d, const char *boot) {
    if (!d || d->index != 3 || !boot || strlen(boot) != 36 || d->service_stage)
        return -1;
    gstd_deployment_workload *w = &d->workloads[0];
    memcpy(d->service_boot, boot, 37);
    if (w->exited && !w->launched)
        return 0;
    if (w->launched && w->exited && w->recovered_stopped && w->observed.stopped &&
        gstd_native_population_empty(&w->population) == 1) {
        d->service_stage = FINISHED;
        d->service_outcome = -1;
        return 0;
    }
    if (!w->launched || gstd_native_watch_poll(w->death_fd) != 0 ||
        gstd_native_population_matches(&w->population, w->resource, w->native.expected_pid,
                                       w->native.expected_start_time, boot))
        return -1;
    d->service_stage = FINISHED;
    d->service_outcome = w->observed.running ? 0 : -1;
    return 0;
}
int gstd_datastore_graph_step(gstd_datastore *d, gstd_bootstrap_stage stage, int result) {
    if (!d || d->index != 3 || d->outcome || !d->environment || !d->environment->database ||
        !sqlite3_get_autocommit(d->environment->database) ||
        (result >= 0 && stage != GSTD_BOOT_PREPARE && gstd_datastore_service_live(d)))
        return -1;
    sqlite3 *db = d->environment->database;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    gstd_bootstrap_graph next = d->graph;
    if (commit(db, gstd_deployment_graph_step(db, d->target, d->configuration_id, &next, stage,
                                              result, NULL)))
        return -1;
    d->graph = next;
    return 0;
}
int gstd_datastore_service_begin(gstd_datastore *d, const gstd_network_namespace *network,
                                 const char *boot, int resolver, int output, int error) {
    if (!d || d->index != 3 || d->outcome || !d->environment || !d->environment->database ||
        !d->target || d->service_stage || d->helper.stage || !network || !boot ||
        strlen(boot) != 36 || resolver < 3 || (output != -1 && output < 3) ||
        (error != -1 && error < 3) ||
        (!d->target->migration && d->graph.stage != GSTD_BOOT_LAUNCH_DATASTORE) ||
        d->graph.outcome || d->volume.writer_fd < 3 || d->volume.data_fd < 3 ||
        !sqlite3_get_autocommit(d->environment->database))
        return -1;
    gstd_deployment_workload *w = &d->workloads[0];
    if (w->native.configuration || w->launched || w->rootfs_fd < 3 ||
        w->scope.descriptor_count != 3)
        return -1;
    memcpy(d->service_boot, boot, 37);
    w->inputs = (gstd_execution_inputs){.manifest = d->target->manifest,
                                        .component = "scylladb",
                                        .purpose = GSTD_EXEC_SERVICE,
                                        .rootfs_fd = w->rootfs_fd,
                                        .network = network};
    for (unsigned i = 0; i < GSTD_MOUNT_COUNT; i++)
        w->inputs.role_fds[i] = -1;
    w->inputs.role_fds[GSTD_MOUNT_DATASTORE] = d->volume.data_fd;
    w->inputs.role_fds[GSTD_MOUNT_RESOLVER] = resolver;
    w->native = (gstd_native_context){.backend_id = w->resource,
                                      .component = "scylladb",
                                      .boot_id = d->service_boot,
                                      .state_fd = d->environment->native_fd,
                                      .bundle_fd = w->scope.descriptors[2],
                                      .rootfs_fd = w->rootfs_fd,
                                      .stdout_fd = output,
                                      .stderr_fd = error,
                                      .writer_lease_fd = d->volume.writer_fd,
                                      .control_owner = getpid(),
                                      .registered_inputs = &w->inputs};
    w->native.configuration = gstd_execution_build(&w->inputs);
    sqlite3 *db = d->environment->database;
    if (!w->native.configuration)
        return -1;
    int failed = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK;
    if (!failed)
        failed = commit(
            db, (!d->target->migration &&
                 gstd_deployment_graph_matches(db, d->target, d->configuration_id, &d->graph)) ||
                    gstd_deployment_native_record(db, d->target, &w->native, "allocated"));
    if (failed) {
        json_decref((json_t *)w->native.configuration);
        w->native.configuration = NULL;
        return -1;
    }
    d->service_stage = CREATING;
    if (gstd_native_launch(&w->native, GSTD_NATIVE_CREATE, 20000, &d->service_job))
        finish(d, -1);
    return 0;
}
int gstd_datastore_service_poll(gstd_datastore *d, int cancellation) {
    if (!d || !d->service_stage || !d->environment || !d->environment->database ||
        (cancellation != -1 && cancellation < 3) ||
        !sqlite3_get_autocommit(d->environment->database))
        return -1;
    if (d->service_stage == FINISHED)
        return 1;
    gstd_deployment_workload *w = &d->workloads[0];
    gstd_native_status status;
    int ready = gstd_native_poll(&d->service_job, cancellation, &status, &w->observed);
    if (ready != 1)
        return ready;
    if (status != GSTD_NATIVE_OK)
        return finish(d, status == GSTD_NATIVE_UNCERTAIN ? -2 : -1);
    sqlite3 *db = d->environment->database;
    if (d->service_stage == CREATING) {
        w->native.expected_pid = w->observed.pid;
        w->native.expected_start_time = w->observed.start_time;
        w->launched = 1;
        if (gstd_native_population_open(w->resource, w->native.expected_pid,
                                        w->native.expected_start_time, d->service_boot,
                                        &w->population) ||
            gstd_native_population_empty(&w->population) != 0 ||
            gstd_native_watch_open(w->native.expected_pid, w->native.expected_start_time,
                                   d->service_boot, &w->death_fd))
            return finish(d, -2);
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK ||
            commit(db, gstd_deployment_native_record(db, d->target, &w->native, "allocated") ||
                           gstd_deployment_population_record(db, d->target, &w->native,
                                                             &w->population)))
            return finish(d, -2);
        d->service_stage = STARTING;
        if (gstd_native_launch(&w->native, GSTD_NATIVE_START, 10000, &d->service_job))
            return finish(d, -2);
        return 0;
    }
    if (d->service_stage == STARTING) {
        d->service_stage = OBSERVING;
        if (gstd_native_launch(&w->native, GSTD_NATIVE_OBSERVE, 10000, &d->service_job))
            return finish(d, -2);
        return 0;
    }
    if (!w->observed.running || gstd_native_watch_poll(w->death_fd) != 0 ||
        gstd_native_population_matches(&w->population, w->resource, w->native.expected_pid,
                                       w->native.expected_start_time, d->service_boot))
        return finish(d, -2);
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return finish(d, -2);
    gstd_bootstrap_graph next = d->graph;
    if (commit(db, gstd_deployment_native_record(db, d->target, &w->native, "running") ||
                       (!d->target->migration &&
                        gstd_deployment_graph_step(db, d->target, d->configuration_id, &next,
                                                   GSTD_BOOT_LAUNCH_DATASTORE, 0, NULL))))
        return finish(d, -2);
    d->graph = next;
    close(d->volume.writer_fd);
    d->volume.writer_fd = -1;
    w->native.writer_lease_fd = -1;
    return finish(d, 0);
}
