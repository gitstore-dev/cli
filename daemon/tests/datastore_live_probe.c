#define _GNU_SOURCE 1
#include "bootstrap_material.h"
#include "datastore.h"
#include "native_exit.h"
#include "network_records.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "datastore live line %d\n", __LINE__);                                 \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int64_t milliseconds(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now)
               ? -1
               : (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static void pause_ms(void) {
    struct timespec delay = {0, 10000000};
    nanosleep(&delay, NULL);
}
static int filesystem(gstd_filesystem_input *input, gstd_filesystem_reply *reply) {
    gstd_filesystem_job job = {0};
    if (gstd_filesystem_launch(input, 10000, &job))
        return -1;
    int ready;
    while (!(ready = gstd_filesystem_poll(&job, -1, reply)))
        pause_ms();
    return ready == 1 && !reply->status ? 0 : -1;
}
static int native(const gstd_native_context *context, gstd_native_action action) {
    gstd_native_job job = {0};
    gstd_native_status status;
    gstd_native_observation observation;
    if (gstd_native_launch(context, action, 20000, &job))
        return -1;
    int ready;
    while (!(ready = gstd_native_poll(&job, -1, &status, &observation)))
        pause_ms();
    return ready == 1 && status == GSTD_NATIVE_OK ? 0 : -1;
}
/* Developer cleanup only: retain every SQL owner and uncertain operation.
 * Never use this signaling path for a keyspace/schema mutator. */
static int stop(const gstd_native_context *context, int death, gstd_native_population *population) {
    if (context->expected_pid <= 0)
        return -1;
    if (gstd_native_watch_poll(death) == 0 && native(context, GSTD_NATIVE_TERM))
        return -1;
    gstd_native_exit exited = {0};
    int64_t deadline = milliseconds() + 30000;
    while (gstd_native_watch_poll(death) == 0 && milliseconds() < deadline) {
        (void)gstd_native_exit_poll(context->expected_pid, context->expected_start_time, &exited);
        pause_ms();
    }
    if (gstd_native_watch_poll(death) == 0) {
        if (native(context, GSTD_NATIVE_KILL))
            return -1;
        deadline = milliseconds() + 5000;
        while (gstd_native_watch_poll(death) == 0 && milliseconds() < deadline) {
            (void)gstd_native_exit_poll(context->expected_pid, context->expected_start_time,
                                        &exited);
            pause_ms();
        }
    }
    (void)gstd_native_exit_poll(context->expected_pid, context->expected_start_time, &exited);
    return gstd_native_watch_poll(death) == 1 && gstd_native_population_empty(population) == 1 &&
                   !native(context, GSTD_NATIVE_DELETE)
               ? 0
               : -1;
}
static int bootstrap(gstd_datastore *d, const gstd_filesystem_reply *config) {
    pid_t owner = getpid(), child = fork();
    if (!child) {
        gstd_credential material = {0};
        gstd_bootstrap_request request = {.manifest = d->target->manifest,
                                          .store_id = d->target->store_id,
                                          .deployment_id = d->target->deployment_id,
                                          .operation_id = d->target->operation_id,
                                          .datastore = "scylladb",
                                          .generation = d->target->generation,
                                          .control_owner = owner,
                                          .scope_fd = config->descriptors[1]};
        int fd = openat(d->environment->state_fd, "fixture-bootstrap-credential",
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        int ok = fd >= 3 && !gstd_bootstrap_material_generate(owner, &material) &&
                 write(fd, material.bytes, material.length) == (ssize_t)material.length &&
                 !fsync(fd) && !fsync(d->environment->state_fd) &&
                 !gstd_bootstrap_prepare(&request, &material);
        gstd_credential_clear(&material);
        if (fd >= 0)
            close(fd);
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                   !WEXITSTATUS(status)
               ? 0
               : -1;
}
static int helper_cleanup(gstd_helper *helper) {
    if (!helper->may_exist)
        return 0;
    if (helper->context.registered_inputs->purpose == GSTD_EXEC_SCYLLA_READY)
        return stop(&helper->context, helper->death_fd, &helper->population);
    /* Unknown mutation completion is never permission to signal the helper or
     * stop its datastore. Observe natural exit, then delete without force. The
     * original uncertain journal/outcome remains unchanged. */
    int64_t deadline = milliseconds() + 70000;
    gstd_native_exit exited = {0};
    while (gstd_native_watch_poll(helper->death_fd) == 0 && milliseconds() < deadline) {
        (void)gstd_native_exit_poll(helper->context.expected_pid,
                                    helper->context.expected_start_time, &exited);
        pause_ms();
    }
    (void)gstd_native_exit_poll(helper->context.expected_pid, helper->context.expected_start_time,
                                &exited);
    return gstd_native_watch_poll(helper->death_fd) == 1 &&
                   gstd_native_population_empty(&helper->population) == 1 &&
                   !native(&helper->context, GSTD_NATIVE_DELETE)
               ? 0
               : -1;
}
static int network_stop(gstd_network_worker *network) {
    if (network->pid <= 0)
        return 0;
    if (!network->permitted) {
        close(network->channel);
        network->channel = -1;
    } else if (gstd_network_worker_signal(network, 0))
        return -1;
    int status = 0;
    for (unsigned force = 0; force < 2; force++) {
        int64_t deadline = milliseconds() + 5000;
        while (milliseconds() < deadline) {
            pid_t result = waitpid(network->pid, &status, WNOHANG);
            if (result == network->pid)
                return 0;
            if (result < 0)
                return -1;
            pause_ms();
        }
        if (!network->permitted || gstd_network_worker_signal(network, 1))
            return -1;
    }
    return -1;
}
int gstd_test_datastore_live(gstd_datastore *d, const gstd_filesystem_reply *config) {
    int result = -1, resolver = -1, output = -1;
    sqlite3 *db = d->environment->database;
    gstd_network_worker network = GSTD_NETWORK_WORKER_INITIAL;
    gstd_network_worker_observation observed = GSTD_NETWORK_OBSERVATION_INITIAL;
    gstd_filesystem_reply scope = {0}, dns = {0};
    char id[65], boot[37];
    REQUIRE(snprintf(id, sizeof(id), "net-%s", d->target->operation_id) > 0 &&
            !gstd_native_boot_id(boot));
    REQUIRE(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    int initial = 0;
    REQUIRE(!gstd_deployment_credential_reserve(db, d->target, d->configuration_id, &initial) &&
            initial && !gstd_deployment_resource_reserve(db, d->target, id, "network") &&
            sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(!bootstrap(d, config) && !gstd_datastore_graph_step(d, GSTD_BOOT_PREPARE, 0));
    gstd_filesystem_input input = {.action = GSTD_FS_SCOPE,
                                   .store_id = d->target->store_id,
                                   .operation_id = d->target->operation_id,
                                   .deployment_id = d->target->deployment_id,
                                   .generation = d->target->generation,
                                   .resource_id = id,
                                   .resource_kind = "network",
                                   .descriptors = {d->environment->resource_fd, -1, -1}};
    REQUIRE(!filesystem(&input, &scope));
    gstd_network_worker_request request = {.manifest = d->target->manifest,
                                           .store_id = d->target->store_id,
                                           .resource_id = id,
                                           .operation_id = d->target->operation_id,
                                           .scope_fd = scope.descriptors[1]};
    REQUIRE(gstd_network_worker_prepare(&request, &network) == GSTD_NETWORK_OK);
    REQUIRE(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK &&
            !gstd_network_record_bind(db, &request, &network) &&
            sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK &&
            !gstd_network_worker_permit(&network));
    int ready = 0;
    int64_t deadline = milliseconds() + 10000;
    while (!ready && milliseconds() < deadline) {
        ready = gstd_network_worker_ready(&network, &observed);
        if (!ready)
            pause_ms();
    }
    REQUIRE(ready == 1 && sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK &&
            !gstd_network_record_running(db, &network, &observed));
    for (unsigned i = 0; i < 3; i++)
        REQUIRE(
            !gstd_network_attachment_reserve(db, &network, &observed, d->workloads[i].resource));
    REQUIRE(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    input = (gstd_filesystem_input){.action = GSTD_FS_RESOLVER,
                                    .descriptors = {config->descriptors[0], -1, -1}};
    REQUIRE(!filesystem(&input, &dns) && dns.descriptor_count == 1);
    resolver = dns.descriptors[0];
    output = openat(d->environment->state_fd, "datastore-live.log",
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    REQUIRE(output >= 3 &&
            !gstd_datastore_service_begin(d, &observed.network, boot, resolver, output, output));
    ready = 0;
    while (!ready) {
        ready = gstd_datastore_service_poll(d, -1);
        REQUIRE(ready >= 0);
        if (!ready)
            pause_ms();
    }
    REQUIRE(!d->service_outcome && d->volume.writer_fd == -1 &&
            d->graph.stage == GSTD_BOOT_WAIT_DATASTORE);
    deadline = milliseconds() + 120000;
    for (unsigned attempt = 0; attempt < 128 && milliseconds() < deadline; attempt++) {
        REQUIRE(!gstd_datastore_helper_begin(d, GSTD_EXEC_SCYLLA_READY, &observed.network, boot,
                                             resolver, output, output));
        ready = 0;
        while (!ready) {
            ready = gstd_datastore_helper_poll(d, -1);
            REQUIRE(ready >= 0);
            if (!ready)
                pause_ms();
        }
        int outcome = d->helper.outcome;
        if (outcome == -2) {
            (void)gstd_datastore_graph_step(d, GSTD_BOOT_WAIT_DATASTORE, -2);
            goto done;
        }
        REQUIRE(outcome == 0 || outcome == -1);
        REQUIRE(!gstd_datastore_graph_step(d, GSTD_BOOT_WAIT_DATASTORE, outcome ? 1 : 0));
        REQUIRE(!gstd_datastore_helper_close(d));
        if (!outcome) {
            result = 0;
            break;
        }
        pause_ms();
    }
    REQUIRE(!result);
    result = -1;
    const gstd_execution_purpose purposes[] = {GSTD_EXEC_SCYLLA_KEYSPACE, GSTD_EXEC_SCHEMA_MIGRATE};
    const gstd_bootstrap_stage stages[] = {GSTD_BOOT_PREPARE_KEYSPACE, GSTD_BOOT_MIGRATE_SCHEMA};
    for (unsigned i = 0; i < 2; i++) {
        REQUIRE(d->graph.stage == stages[i] &&
                !gstd_datastore_helper_begin(d, purposes[i], &observed.network, boot, resolver,
                                             output, output));
        ready = 0;
        while (!ready) {
            ready = gstd_datastore_helper_poll(d, -1);
            REQUIRE(ready >= 0);
            if (!ready)
                pause_ms();
        }
        int outcome = d->helper.outcome;
        REQUIRE(!gstd_datastore_graph_step(d, stages[i], outcome));
        REQUIRE(!outcome && !gstd_datastore_helper_close(d));
    }
    REQUIRE(d->graph.stage == GSTD_BOOT_LAUNCH_GIT && !d->graph.outcome);
    result = 0;
done:
    if (!sqlite3_get_autocommit(db))
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    int safe = 1;
    if (d->helper.stage) {
        gstd_helper *helper = &d->helper.helper;
        if (helper_cleanup(helper))
            safe = 0;
        if (gstd_datastore_helper_close(d))
            safe = 0;
    }
    gstd_deployment_workload *service = &d->workloads[0];
    if (service->launched) {
        if (!safe || stop(&service->native, service->death_fd, &service->population))
            safe = 0;
        if (safe) {
            close(service->death_fd);
            service->death_fd = -1;
            gstd_native_population_close(&service->population);
            service->launched = 0;
        }
    } else if (d->service_stage)
        safe = 0;
    if (safe) {
        json_decref((json_t *)service->native.configuration);
        service->native.configuration = NULL;
        d->service_stage = 0;
        if (network_stop(&network))
            safe = 0;
    }
    if (safe) {
        gstd_network_worker_observation_close(&observed);
        gstd_network_worker_close(&network);
    }
    gstd_filesystem_reply_close(&dns);
    gstd_filesystem_reply_close(&scope);
    if (output >= 0)
        close(output);
    if (!safe)
        fputs("datastore fixture retained: native cleanup uncertain\n", stderr);
    if (!result && safe)
        puts("{\"journaledReadiness\":true,\"journaledKeyspace\":true,\"journaledMigration\":true,"
             "\"datastoreStopped\":true,\"publicCapability\":false}");
    return !result && safe ? 0 : -1;
}
