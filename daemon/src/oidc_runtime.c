#define _GNU_SOURCE 1
#include "oidc_runtime.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "native_exit.h"
#include "network_records.h"
#include "oidc_profile.h"
#include "volume_records.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
enum { PREPARING = 1, SCOPE, ASSET, MATERIALIZING, PREPARED };
enum { MIGRATING = 1, CREATING, STARTING, OBSERVING, HEALTH, ENROLLING, FINISHED };
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int sql(gstd_oidc_runtime *d, const char *query) {
    return sqlite3_exec(d->environment->database, query, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
static int commit(gstd_oidc_runtime *d, int failed) {
    if (failed || sql(d, "COMMIT")) {
        sql(d, "ROLLBACK");
        return -1;
    }
    return 0;
}
static int prepared(gstd_oidc_runtime *d, int outcome, int detail) {
    d->stage = PREPARED;
    d->outcome = outcome;
    d->detail = detail;
    return 1;
}
static int finished(gstd_oidc_runtime *d, int outcome) {
    d->execute_stage = FINISHED;
    d->execute_outcome = outcome;
    if (!outcome && d->final_check)
        d->final_verified = 1;
    return 1;
}
static int cancelled(int fd) {
    struct pollfd notification = {.fd = fd, .events = POLLIN};
    return fd >= 3 && poll(&notification, 1, 0) > 0 && notification.revents;
}
static gstd_filesystem_input input(gstd_oidc_runtime *d, gstd_filesystem_action action,
                                   int destination, int source) {
    return (gstd_filesystem_input){.action = action,
                                   .manifest = d->target->manifest,
                                   .store_id = d->target->store_id,
                                   .deployment_id = d->target->deployment_id,
                                   .operation_id = d->target->operation_id,
                                   .generation = d->target->generation,
                                   .descriptors = {destination, source, -1}};
}
static int scope_launch(gstd_oidc_runtime *d) {
    gstd_deployment_workload *w = &d->workloads[d->index];
    if (sql(d, "BEGIN IMMEDIATE"))
        return prepared(d, -2, 0);
    if (commit(d, gstd_deployment_resource_reserve(d->environment->database, d->target, w->resource,
                                                   "workload")))
        return prepared(d, -2, 0);
    gstd_filesystem_input request = input(d, GSTD_FS_SCOPE, d->environment->resource_fd, -1);
    request.resource_id = w->resource;
    request.resource_kind = "workload";
    d->stage = SCOPE;
    return gstd_filesystem_launch(&request, 10000, &d->filesystem) ? prepared(d, -1, 0) : 0;
}
static int seed_workloads(const gstd_deployment_environment *environment,
                          const gstd_deployment_target *target, gstd_oidc_runtime *d) {
    *d = (gstd_oidc_runtime){.environment = environment, .target = target, .resolver_fd = -1};
    const char *names[] = {"oidc-smtp",   "oidc-hydra", "oidc-kratos", "oidc-ui",
                           "oidc-bridge", "oidc-hydra", "oidc-kratos"};
    for (unsigned i = 0; i < GSTD_OIDC_WORKLOAD_COUNT; i++) {
        gstd_deployment_workload *w = &d->workloads[i];
        w->name = names[i];
        w->rootfs_fd = w->death_fd = -1;
        w->population = (gstd_native_population)GSTD_NATIVE_POPULATION_INITIAL;
        int n = snprintf(w->resource, sizeof(w->resource), "%s-%s-%s",
                         i < GSTD_OIDC_SERVICE_COUNT ? "run" : "migrate", names[i],
                         target->operation_id);
        int m = snprintf(w->asset_resource, sizeof(w->asset_resource), "%s-%s-%s",
                         i < GSTD_OIDC_SERVICE_COUNT ? "asset" : "asset-migrate", names[i],
                         target->operation_id);
        if (n < 1 || n >= (int)sizeof(w->resource) || m < 1 || m >= (int)sizeof(w->asset_resource))
            return -1;
    }
    for (unsigned i = 0; i < GSTD_OIDC_SERVICE_COUNT; i++)
        d->output[i][0] = d->output[i][1] = -1;
    return 0;
}
int gstd_oidc_runtime_recovery_seed(const gstd_deployment_environment *environment,
                                    const gstd_deployment_target *target, gstd_oidc_runtime *d) {
    if (!environment || !target || !d || d->stage || !environment->database ||
        gstd_oidc_core_manifest_check(target->manifest) ||
        !json_object_get(target->manifest, "oidcProfileDigest") ||
        seed_workloads(environment, target, d))
        return -1;
    gstd_oidc_setup *setup = &d->setup;
    setup->environment = environment;
    setup->target = target;
    int n = snprintf(setup->configuration_id, sizeof(setup->configuration_id), "oidc-cfg-%s",
                     target->operation_id);
    if (n < 1 || n >= (int)sizeof(setup->configuration_id))
        return -1;
    for (unsigned i = 0; i < 4; i++)
        setup->files[i] = -1;
    for (unsigned i = 0; i < 2; i++) {
        setup->volumes[i].data_fd = setup->volumes[i].writer_fd = -1;
        if (gstd_oidc_volume_record_recovery_read(environment->database, &environment->volume_root,
                                                  (gstd_oidc_volume_role)i, target->operation_id,
                                                  setup->volume_ids[i], setup->producers[i],
                                                  &setup->volumes[i].identity))
            return -1;
    }
    d->stage = PREPARED;
    d->outcome = d->execute_outcome = -2;
    d->execute_stage = FINISHED;
    return 0;
}
int gstd_oidc_runtime_prepare_begin(const gstd_deployment_environment *environment,
                                    const gstd_deployment_target *target,
                                    const gstd_oidc_ports *ports, gstd_oidc_runtime *d) {
    if (!environment || !environment->database || !environment->settings || !target || !d ||
        d->stage || environment->cache_fd < 3 || environment->resource_fd < 3 ||
        environment->native_fd < 3 || !sqlite3_get_autocommit(environment->database) ||
        gstd_oidc_core_manifest_check(target->manifest) ||
        !json_object_get(target->manifest, "oidcProfileDigest") || gstd_oidc_ports_check(ports))
        return -1;
    if (seed_workloads(environment, target, d))
        return -1;
    json_t *identity =
        json_pack("{s:s,s:s}", "storeId", target->store_id, "deploymentId", target->deployment_id);
    char hash[65];
    int valid = identity && !gstd_fingerprint(identity, hash);
    json_decref(identity);
    if (!valid)
        return -1;
    snprintf(d->client_id, sizeof(d->client_id), "gitstore-%.48s", hash);
    snprintf(d->redirect_uri, sizeof(d->redirect_uri), "http://127.0.0.1:%u/callback", ports->ui);
    d->client = (gstd_oidc_client){d->client_id, "gitstore", d->redirect_uri};
    if (gstd_oidc_client_check(&d->client) ||
        gstd_oidc_setup_begin(environment, target, ports, &d->setup))
        return -1;
    d->stage = PREPARING;
    return 0;
}
int gstd_oidc_runtime_prepare_poll(gstd_oidc_runtime *d, int cancellation) {
    if (!d || !d->stage || !sqlite3_get_autocommit(d->environment->database) ||
        (cancellation != -1 && cancellation < 3))
        return -1;
    if (d->stage == PREPARED)
        return 1;
    if (d->stage == PREPARING) {
        int ready = gstd_oidc_setup_poll(&d->setup, cancellation);
        if (ready != 1)
            return ready;
        return d->setup.outcome ? prepared(d, d->setup.outcome, d->setup.detail) : scope_launch(d);
    }
    gstd_deployment_workload *w = &d->workloads[d->index];
    if (d->stage == ASSET) {
        int ready = gstd_deployment_asset_poll(&d->asset, cancellation);
        if (ready != 1)
            return ready;
        if (d->asset.outcome)
            return prepared(d, d->asset.outcome, d->asset.detail);
        gstd_filesystem_input request =
            input(d, GSTD_FS_IMAGE_MATERIALIZE, w->scope.descriptors[1], d->asset.active_fd);
        request.component = w->name;
        request.download_limit = UINT64_C(512) * 1024 * 1024;
        request.merge_limit = UINT64_C(2) * 1024 * 1024 * 1024;
        request.image_timeout_ms = 180000;
        d->stage = MATERIALIZING;
        return gstd_filesystem_launch(&request, 190000, &d->filesystem) ? prepared(d, -1, 0) : 0;
    }
    gstd_filesystem_reply reply = {0};
    int ready = gstd_filesystem_poll(&d->filesystem, cancellation, &reply);
    if (ready != 1)
        return ready;
    if (reply.status) {
        int status = reply.status, detail = reply.detail;
        gstd_filesystem_reply_close(&reply);
        return prepared(d, status == -2 ? -2 : -1, detail);
    }
    if (d->stage == SCOPE) {
        w->scope = reply;
        if (sql(d, "BEGIN IMMEDIATE"))
            return prepared(d, -2, 0);
        if (commit(d, gstd_deployment_scope_record(d->environment->database, d->target, w->resource,
                                                   &reply) ||
                          gstd_deployment_resource_reserve(d->environment->database, d->target,
                                                           w->asset_resource, "asset-root")))
            return prepared(d, -2, 0);
        d->stage = ASSET;
        if (gstd_deployment_asset_begin(
                d->environment->database, d->target, w->name, w->asset_resource,
                d->environment->cache_fd,
                gstd_runtime_settings_image(d->environment->settings, w->name),
                UINT64_C(512) * 1024 * 1024, UINT64_C(2) * 1024 * 1024 * 1024,
                UINT64_C(16) * 1024 * 1024 * 1024, &d->asset))
            return prepared(d, -1, 0);
        return 0;
    }
    if (d->stage != MATERIALIZING || reply.descriptor_count != 1) {
        gstd_filesystem_reply_close(&reply);
        return prepared(d, -2, 0);
    }
    w->rootfs_fd = reply.descriptors[0];
    if (sql(d, "BEGIN IMMEDIATE"))
        return prepared(d, -2, 0);
    if (commit(d, gstd_deployment_execution_record(d->environment->database, d->target, w->resource,
                                                   &w->scope, w->rootfs_fd)) ||
        gstd_deployment_asset_close(&d->asset))
        return prepared(d, -2, 0);
    return ++d->index < GSTD_OIDC_WORKLOAD_COUNT ? scope_launch(d) : prepared(d, 0, 0);
}
static int inputs(gstd_oidc_runtime *d, unsigned index, int migration) {
    gstd_deployment_workload *w = &d->workloads[index];
    int hydra = !strcmp(w->name, "oidc-hydra"), kratos = !strcmp(w->name, "oidc-kratos");
    w->inputs =
        (gstd_execution_inputs){.manifest = d->target->manifest,
                                .component = w->name,
                                .purpose = migration ? GSTD_EXEC_OIDC_MIGRATE : GSTD_EXEC_SERVICE,
                                .rootfs_fd = w->rootfs_fd,
                                .network = d->network,
                                .oidc_ports = &d->setup.ports};
    for (unsigned i = 0; i < GSTD_MOUNT_COUNT; i++)
        w->inputs.role_fds[i] = -1;
    w->inputs.role_fds[GSTD_MOUNT_RESOLVER] = d->resolver_fd;
    if (hydra || kratos) {
        w->inputs.role_fds[GSTD_MOUNT_DATASTORE] = d->setup.volumes[kratos ? 1 : 0].data_fd;
        if (!migration)
            w->inputs.role_fds[GSTD_MOUNT_OVERLAY] = d->setup.files[kratos ? 1 : 0];
        if (kratos && !migration)
            w->inputs.role_fds[GSTD_MOUNT_POLICY] = d->setup.files[2];
    } else if (!strcmp(w->name, "oidc-ui"))
        w->inputs.role_fds[GSTD_MOUNT_OVERLAY] = d->setup.files[3];
    unsigned service = migration ? index - GSTD_OIDC_SERVICE_COUNT + 1 : index;
    w->native = (gstd_native_context){
        .backend_id = w->resource,
        .component = w->name,
        .boot_id = d->boot,
        .state_fd = d->environment->native_fd,
        .bundle_fd = w->scope.descriptors[2],
        .rootfs_fd = w->rootfs_fd,
        .stdout_fd = d->output[service][0],
        .stderr_fd = d->output[service][1],
        .writer_lease_fd = hydra || kratos ? d->setup.volumes[kratos ? 1 : 0].writer_fd : -1,
        .control_owner = getpid(),
        .registered_inputs = &w->inputs};
    w->native.configuration = gstd_execution_build(&w->inputs);
    return w->native.configuration ? 0 : -1;
}
int gstd_oidc_runtime_service_live(const gstd_oidc_runtime *d, unsigned index) {
    if (!d || index >= GSTD_OIDC_SERVICE_COUNT || !d->network || !d->network_owner ||
        gstd_native_watch_poll(d->network_owner->pid_fd) != 0 ||
        gstd_network_namespace_check(d->network))
        return -1;
    const gstd_deployment_workload *w = &d->workloads[index];
    if (!w->launched || gstd_native_watch_poll(w->death_fd) != 0 ||
        gstd_native_population_matches(&w->population, w->resource, w->native.expected_pid,
                                       w->native.expected_start_time, d->boot))
        return -2;
    if (index == 1 || index == 2) {
        const gstd_volume_handle *v = &d->setup.volumes[index - 1];
        struct stat writer, data, init_writer;
        char path[80];
        int n = snprintf(path, sizeof(path), "/proc/%ld/fd/3", (long)w->native.expected_pid);
        if (n < 1 || n >= (int)sizeof(path) || v->writer_fd < 3 || v->data_fd < 3 ||
            fstat(v->writer_fd, &writer) || fstat(v->data_fd, &data) || stat(path, &init_writer) ||
            (uint64_t)writer.st_dev != v->identity.writer_device ||
            (uint64_t)writer.st_ino != v->identity.writer_inode ||
            (uint64_t)data.st_dev != v->identity.data_device ||
            (uint64_t)data.st_ino != v->identity.data_inode ||
            init_writer.st_dev != writer.st_dev || init_writer.st_ino != writer.st_ino ||
            !S_ISREG(init_writer.st_mode) || init_writer.st_uid != geteuid() ||
            (init_writer.st_mode & 07777) != 0600 || init_writer.st_nlink != 1 ||
            gstd_native_watch_poll(w->death_fd) != 0)
            return -3;
    }
    return 0;
}
static int current(gstd_oidc_runtime *d) {
    if (!d->network_owner || gstd_native_watch_poll(d->network_owner->pid_fd) != 0 ||
        gstd_network_namespace_check(d->network))
        return -1;
    for (unsigned i = 0; i < d->started; i++)
        if (gstd_oidc_runtime_service_live(d, i))
            return -1;
    if (sql(d, "BEGIN IMMEDIATE"))
        return -1;
    int failed = 0;
    for (unsigned i = 0; !failed && i < GSTD_OIDC_WORKLOAD_COUNT; i++)
        failed = gstd_network_attachment_matches(d->environment->database, d->target->operation_id,
                                                 d->workloads[i].resource, d->network);
    return commit(d, failed);
}
static int service_launch(gstd_oidc_runtime *d) {
    if (current(d) || inputs(d, d->index, 0) || sql(d, "BEGIN IMMEDIATE"))
        return finished(d, -2);
    gstd_deployment_workload *w = &d->workloads[d->index];
    if (commit(d, gstd_deployment_native_record(d->environment->database, d->target, &w->native,
                                                "allocated")))
        return finished(d, -2);
    d->execute_stage = CREATING;
    return gstd_native_launch(&w->native, GSTD_NATIVE_CREATE, 20000, &d->native_job)
               ? finished(d, -2)
               : 0;
}
static int migrate(gstd_oidc_runtime *d) {
    unsigned index = GSTD_OIDC_SERVICE_COUNT + d->migrated;
    if (current(d) || inputs(d, index, 1) ||
        gstd_helper_owner_begin(d->environment->database, d->target, d->setup.configuration_id,
                                &d->workloads[index].native, &d->migration))
        return finished(d, -2);
    d->execute_stage = MIGRATING;
    return 0;
}
static gstd_bootstrap_job_input http_input(gstd_oidc_runtime *d) {
    return (gstd_bootstrap_job_input){
        .preparation = {.manifest = d->target->manifest,
                        .store_id = d->target->store_id,
                        .deployment_id = d->target->deployment_id,
                        .operation_id = d->target->operation_id,
                        .generation = d->target->generation,
                        .control_owner = getpid(),
                        .scope_fd = d->setup.configuration.descriptors[1]},
        .network = d->network,
        .oidc_ports = d->setup.ports,
        .oidc_client = d->client,
        .source = d->setup.source,
        .broker_fd = d->environment->broker_fd};
}
static int health(gstd_oidc_runtime *d) {
    if (current(d))
        return finished(d, -2);
    gstd_bootstrap_job_input request = http_input(d);
    request.kind = GSTD_BOOT_JOB_OIDC_HEALTH;
    request.source = NULL;
    request.broker_fd = -1;
    request.oidc_health_role =
        d->index == GSTD_OIDC_SERVICE_COUNT
            ? GSTD_OIDC_HEALTH_ISSUER
            : (gstd_oidc_health_role)gstd_workload_oidc_health((gstd_workload_role)d->index);
    d->execute_stage = HEALTH;
    return gstd_bootstrap_job_launch(&request, 5000, &d->http) ? finished(d, -1) : 0;
}
static int enrollment_launch(gstd_oidc_runtime *d) {
    if (current(d) || sql(d, "BEGIN IMMEDIATE"))
        return finished(d, -2);
    json_t *record = NULL;
    if (commit(d, gstd_deployment_oidc_enrollment_begin(d->environment->database, d->target,
                                                        d->setup.configuration_id, &d->client,
                                                        &record))) {
        json_decref(record);
        return finished(d, -2);
    }
    json_decref(d->enrollment);
    d->enrollment = record;
    const char *state = json_string_value(json_object_get(record, "state"));
    gstd_bootstrap_job_input request = http_input(d);
    request.kind = GSTD_BOOT_JOB_OIDC_HTTP;
    if (state && !strcmp(state, "prepared")) {
        if (current(d) || sql(d, "BEGIN IMMEDIATE"))
            return finished(d, -2);
        record = NULL;
        if (commit(d, gstd_deployment_oidc_enrollment_dispatch(
                          d->environment->database, d->target, d->setup.configuration_id,
                          &d->client, d->enrollment, &record))) {
            json_decref(record);
            return finished(d, -2);
        }
        json_decref(d->enrollment);
        d->enrollment = record;
        request.oidc_action = GSTD_OIDC_HTTP_CREATE;
    } else if (state && !strcmp(state, "created"))
        request.oidc_action = GSTD_OIDC_HTTP_INSPECT;
    else
        return finished(d, -2);
    d->enrollment_action = (int)request.oidc_action;
    d->execute_stage = ENROLLING;
    return gstd_bootstrap_job_launch(&request, 10000, &d->http) ? finished(d, -2) : 0;
}
int gstd_oidc_runtime_execute_begin(gstd_oidc_runtime *d, const gstd_network_namespace *network,
                                    const gstd_network_worker *owner, const char *boot,
                                    int resolver, const int output[GSTD_OIDC_SERVICE_COUNT][2]) {
    if (!d || d->stage != PREPARED || d->outcome || d->execute_stage || !network ||
        gstd_network_namespace_check(network) || !boot || strlen(boot) != 36 ||
        strcmp(boot, network->boot_id) || !owner || strcmp(boot, owner->boot_id) ||
        gstd_native_watch_poll(owner->pid_fd) != 0 || resolver < 3 ||
        !sqlite3_get_autocommit(d->environment->database))
        return -1;
    d->network = network;
    d->network_owner = owner;
    d->resolver_fd = resolver;
    memcpy(d->boot, boot, 37);
    for (unsigned i = 0; i < GSTD_OIDC_SERVICE_COUNT; i++)
        for (unsigned j = 0; j < 2; j++) {
            int fd = output ? output[i][j] : -1;
            if (fd != -1 && fd < 3)
                return -1;
            d->output[i][j] = fd;
        }
    migrate(d);
    return 0;
}
int gstd_oidc_runtime_final_health_begin(gstd_oidc_runtime *d) {
    if (!d || d->execute_stage != FINISHED || d->execute_outcome || d->final_check ||
        d->http.worker || d->native_job.worker || d->migration.stage || d->cancel_requested)
        return -1;
    d->final_check = 1;
    d->execute_outcome = -2;
    d->sweeping = 1;
    d->index = 0;
    d->deadline_ms = now_ms() + 30000;
    d->next_health_ms = 0;
    return health(d);
}
int gstd_oidc_runtime_execute_poll(gstd_oidc_runtime *d, int cancel) {
    if (!d || !d->execute_stage || (cancel != -1 && cancel < 3) ||
        !sqlite3_get_autocommit(d->environment->database))
        return -1;
    if (d->execute_stage == FINISHED)
        return 1;
    d->cancel_requested |= cancelled(cancel);
    if (d->execute_stage == MIGRATING) {
        int ready = gstd_helper_owner_poll(&d->migration, cancel);
        if (ready != 1)
            return ready;
        if (d->migration.outcome) {
            int outcome = d->migration.outcome;
            if (outcome == -1 && d->migration.helper.deleted && !d->migration.helper.may_exist)
                (void)gstd_helper_owner_close(&d->migration);
            return finished(d, outcome);
        }
        if (gstd_helper_owner_close(&d->migration))
            return finished(d, -2);
        if (d->cancel_requested)
            return finished(d, -1);
        if (++d->migrated < 2)
            return migrate(d);
        d->index = 0;
        return service_launch(d);
    }
    if (d->execute_stage >= CREATING && d->execute_stage <= OBSERVING) {
        gstd_deployment_workload *w = &d->workloads[d->index];
        gstd_native_status status;
        int ready = gstd_native_poll(&d->native_job, cancel, &status, &w->observed);
        if (ready != 1)
            return ready;
        if (status != GSTD_NATIVE_OK) {
            d->detail = 100 + (int)status;
            return finished(d, -2);
        }
        if (d->execute_stage == CREATING) {
            w->native.stdout_fd = w->native.stderr_fd = -1;
            w->native.expected_pid = w->observed.pid;
            w->native.expected_start_time = w->observed.start_time;
            w->launched = 1;
            if (gstd_native_watch_open(w->native.expected_pid, w->native.expected_start_time,
                                       d->boot, &w->death_fd) ||
                gstd_native_population_open(w->resource, w->native.expected_pid,
                                            w->native.expected_start_time, d->boot,
                                            &w->population) ||
                sql(d, "BEGIN IMMEDIATE"))
                return finished(d, -2);
            if (commit(d, gstd_deployment_native_record(d->environment->database, d->target,
                                                        &w->native, "allocated") ||
                              gstd_deployment_population_record(d->environment->database, d->target,
                                                                &w->native, &w->population)))
                return finished(d, -2);
            if (d->cancel_requested)
                return finished(d, -1);
            d->execute_stage = STARTING;
            return gstd_native_launch(&w->native, GSTD_NATIVE_START, 10000, &d->native_job)
                       ? finished(d, -2)
                       : 0;
        }
        if (d->execute_stage == STARTING) {
            d->execute_stage = OBSERVING;
            return gstd_native_launch(&w->native, GSTD_NATIVE_OBSERVE, 10000, &d->native_job)
                       ? finished(d, -2)
                       : 0;
        }
        int live = gstd_oidc_runtime_service_live(d, d->index);
        if (!w->observed.running || live || sql(d, "BEGIN IMMEDIATE")) {
            d->detail = !w->observed.running ? 200 : live ? 210 - live : 220;
            return finished(d, -2);
        }
        int failed = gstd_deployment_native_record(d->environment->database, d->target, &w->native,
                                                   "running");
        sqlite3_stmt *component = NULL;
        if (!failed)
            failed =
                sqlite3_prepare_v2(d->environment->database,
                                   "UPDATE components SET backend_id=?,observed_state='running',"
                                   "observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE "
                                   "deployment_id=? "
                                   "AND component=? AND generation=?",
                                   -1, &component, NULL) != SQLITE_OK;
        if (!failed) {
            sqlite3_bind_text(component, 1, w->resource, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(component, 2, d->target->deployment_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(component, 3, w->name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(component, 4, (sqlite3_int64)d->target->generation);
            failed = sqlite3_step(component) != SQLITE_DONE ||
                     sqlite3_changes(d->environment->database) != 1;
        }
        sqlite3_finalize(component);
        if (commit(d, failed))
            return finished(d, -2);
        d->started = d->index + 1;
        if (d->cancel_requested)
            return finished(d, -1);
        d->deadline_ms = now_ms() + 120000;
        return health(d);
    }
    if (d->execute_stage == HEALTH || d->execute_stage == ENROLLING) {
        if (!d->http.worker) {
            if (d->cancel_requested)
                return finished(d, -1);
            return now_ms() < d->next_health_ms ? 0 : health(d);
        }
        gstd_bootstrap_job_reply reply = {0};
        int ready = gstd_bootstrap_job_poll(&d->http, cancel, &reply);
        if (ready != 1)
            return ready;
        if (d->execute_stage == ENROLLING) {
            if (d->enrollment_action == GSTD_OIDC_HTTP_CREATE) {
                /* Remember a known original ACK even when cancellation is ready.
                 * This never advances readiness or releases any ownership. */
                json_t *record = NULL;
                if (sql(d, "BEGIN IMMEDIATE"))
                    return finished(d, -2);
                if (commit(d, gstd_deployment_oidc_enrollment_observe(
                                  d->environment->database, d->target, d->setup.configuration_id,
                                  &d->client, d->enrollment, (gstd_oidc_http_status)reply.status,
                                  &record))) {
                    json_decref(record);
                    return finished(d, -2);
                }
                json_decref(d->enrollment);
                d->enrollment = record;
            }
            int success = d->enrollment_action == GSTD_OIDC_HTTP_CREATE ? GSTD_OIDC_CLIENT_CREATED
                                                                        : GSTD_OIDC_PUBLIC_MATCH;
            if (reply.status != success || current(d))
                return finished(d, -2);
            if (d->cancel_requested)
                return finished(d, -1);
            d->index = 3;
            return service_launch(d);
        }
        if (d->cancel_requested)
            return finished(d, reply.status == -2 ? -2 : -1);
        if (current(d))
            return finished(d, -2);
        if (reply.status == GSTD_OIDC_HEALTH_PENDING) {
            if (now_ms() >= d->deadline_ms)
                return finished(d, -1);
            d->next_health_ms = now_ms() + 250;
            return 0;
        }
        if (reply.status != GSTD_OIDC_HEALTH_READY)
            return finished(d, reply.status == -2 ? -2 : -1);
        if (d->sweeping) {
            if (++d->index > GSTD_OIDC_SERVICE_COUNT)
                return finished(d, 0);
            return health(d);
        }
        if (d->index == 2)
            return enrollment_launch(d);
        if (++d->index < GSTD_OIDC_SERVICE_COUNT)
            return service_launch(d);
        d->sweeping = 1;
        d->index = 0;
        d->deadline_ms = now_ms() + 120000;
        return health(d);
    }
    return finished(d, -2);
}
int gstd_oidc_runtime_observers_close(gstd_oidc_runtime *d) {
    if (!d || !d->stage || d->filesystem.worker || d->asset.worker.worker ||
        d->setup.worker.worker || d->native_job.worker || d->http.worker ||
        d->migration.helper.worker.worker)
        return -1;
    if (d->migration.stage && gstd_helper_owner_observers_close(&d->migration))
        return -1;
    if (d->asset.stage && gstd_deployment_asset_close(&d->asset))
        return -1;
    for (unsigned i = 0; i < GSTD_OIDC_WORKLOAD_COUNT; i++) {
        gstd_deployment_workload *w = &d->workloads[i];
        json_decref((json_t *)w->native.configuration);
        if (w->rootfs_fd >= 3)
            close(w->rootfs_fd);
        if (w->death_fd >= 3)
            close(w->death_fd);
        gstd_native_population_close(&w->population);
        gstd_filesystem_reply_close(&w->scope);
    }
    if (d->setup.environment && gstd_oidc_setup_close(&d->setup))
        return -1;
    json_decref(d->enrollment);
    memset(d, 0, sizeof(*d));
    return 0;
}
