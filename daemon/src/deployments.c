#define _GNU_SOURCE 1
#include "deployments.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "host_probe.h"
#include "native_exit.h"
#include "network_records.h"
#include "volume_records.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
enum {
    CONFIG_SCOPE = 1,
    WORK_SCOPE,
    ASSET,
    MATERIALIZE,
    VOLUME,
    NETWORK_SCOPE,
    NETWORK_READY,
    RESOLVER,
    BOOTSTRAP,
    NATIVE_CREATE,
    NATIVE_START,
    NATIVE_OBSERVE,
    HEALTH,
    BIND,
    FINISHED,
    HOST_PREFLIGHT,
    CAPTURE_READY,
    DATASTORE_PREPARE,
    DATASTORE_SERVICE,
    DATASTORE_HELPER,
    OIDC_PORTS,
    OIDC_PREPARE,
    OIDC_EXECUTE,
    OIDC_FINAL_HEALTH
};
unsigned gstd_deployment_service_count(const gstd_deployment *d) {
    return d ? d->count + (d->oidc_enabled ? GSTD_OIDC_SERVICE_COUNT : 0) : 0;
}
gstd_deployment_workload *gstd_deployment_service(gstd_deployment *d, unsigned index) {
    if (!d || index >= gstd_deployment_service_count(d))
        return NULL;
    if (d->oidc_enabled) {
        if (index < GSTD_OIDC_SERVICE_COUNT)
            return &d->oidc.workloads[index];
        index -= GSTD_OIDC_SERVICE_COUNT;
    }
    return &d->workloads[index];
}
unsigned gstd_deployment_owned_workload_count(const gstd_deployment *d) {
    return gstd_deployment_service_count(d) + (d && d->oidc_enabled ? 2u : 0u) +
           (d && d->persistent ? 3u : 0u);
}
gstd_deployment_workload *gstd_deployment_owned_workload(gstd_deployment *d, unsigned index) {
    unsigned services = gstd_deployment_service_count(d);
    if (index < services)
        return gstd_deployment_service(d, index);
    index -= services;
    if (d && d->oidc_enabled) {
        if (index < 2)
            return &d->oidc.workloads[GSTD_OIDC_SERVICE_COUNT + index];
        index -= 2;
    }
    return d && d->persistent && index < 3 ? &d->datastore.workloads[index] : NULL;
}
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int sql(gstd_deployment *d, const char *text) {
    return sqlite3_exec(d->environment->database, text, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
static int commit(gstd_deployment *d, int failure) {
    if (failure || sql(d, "COMMIT")) {
        sql(d, "ROLLBACK");
        return -1;
    }
    return 0;
}
static void bind(sqlite3_stmt *s, int i, const char *text) {
    sqlite3_bind_text(s, i, text, -1, SQLITE_TRANSIENT);
}
static sqlite3_stmt *prepare(gstd_deployment *d, const char *text) {
    sqlite3_stmt *s = NULL;
    return sqlite3_prepare_v2(d->environment->database, text, -1, &s, NULL) == SQLITE_OK ? s : NULL;
}
static int identifier(char out[65], const char *role, const char *operation) {
    int n = snprintf(out, 65, "%s-%s", role, operation);
    return n > 0 && n < 65 ? 0 : -1;
}
static int finish(gstd_deployment *d, int outcome, int detail) {
    if (!outcome && d->oidc_enabled && !d->finalizing && !d->oidc.final_check) {
        d->stage = OIDC_FINAL_HEALTH;
        if (!gstd_oidc_runtime_final_health_begin(&d->oidc))
            return 0;
        outcome = d->oidc.execute_outcome ? d->oidc.execute_outcome : -1;
    }
    if (!outcome && d->oidc.final_check &&
        (!d->oidc.final_verified || now_ms() >= d->oidc.deadline_ms))
        outcome = -1;
    int host_failure =
        d->finalizing ? d->final_host_failure : d->stage == HOST_PREFLIGHT && outcome == -1;
    if (!outcome) {
        if (d->persistent && gstd_datastore_service_live(&d->datastore))
            outcome = -1;
        for (unsigned i = 0; i < d->count; i++)
            if (gstd_native_watch_poll(d->workloads[i].death_fd) != 0)
                outcome = -1;
        for (unsigned i = 0; d->oidc_enabled && i < GSTD_OIDC_SERVICE_COUNT; i++)
            if (d->oidc.execute_outcome || gstd_oidc_runtime_service_live(&d->oidc, i))
                outcome = -1;
        if (gstd_native_watch_poll(d->network.pid_fd) != 0 ||
            gstd_native_watch_poll(d->keeper_death_fd) != 0 || d->cancel_requested)
            outcome = -1;
    }
    d->finalizing = 1;
    d->final_outcome = outcome;
    d->final_detail = detail;
    d->final_host_failure = host_failure;
    gstd_log_capture_outputs_close(&d->capture);
    for (unsigned i = 0; i < 2; i++) {
        if (d->helper_output[i] >= 3)
            close(d->helper_output[i]);
        d->helper_output[i] = -1;
    }
    d->stage = FINISHED;
    d->outcome = outcome;
    d->detail = detail;
    /* Keep the runtime reservation until its immutable result commits. Pending
     * finalization does not repeat native/credential effects or claim readiness.
     */
    if (sql(d, "BEGIN IMMEDIATE")) {
        d->outcome = -2;
        return 0;
    }
    sqlite3_stmt *s =
        prepare(d, "UPDATE deployments SET readiness=?,observed_generation=CASE WHEN ?=0 "
                   "THEN "
                   "generation ELSE observed_generation END,recovery=CASE WHEN ?=-2 THEN "
                   "'uncertain' ELSE recovery "
                   "END,observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') "
                   "WHERE deployment_id=? AND generation=? AND last_operation_id=?");
    int ok = s != NULL;
    if (ok) {
        bind(s, 1, outcome ? "failed" : "ready");
        sqlite3_bind_int(s, 2, outcome);
        sqlite3_bind_int(s, 3, outcome);
        bind(s, 4, d->target.deployment_id);
        sqlite3_bind_int64(s, 5, (sqlite3_int64)d->target.generation);
        bind(s, 6, d->target.operation_id);
        ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(d->environment->database) == 1;
    }
    sqlite3_finalize(s);
    json_t *value =
        outcome
            ? json_pack("{s:s,s:s,s:b,s:s}", "code",
                        host_failure    ? "UNSUPPORTED_HOST"
                        : outcome == -2 ? "OWNERSHIP_UNCERTAIN"
                                        : "OPERATION_FAILED",
                        "stage", "start", "retryable", 0, "message",
                        "Owned start did not reach verified readiness; "
                        "resources retained")
            : json_pack("{s:s,s:I,s:{s:s,s:i,s:s,s:i}}", "deploymentId", d->target.deployment_id,
                        "generation", (json_int_t)d->target.generation, "endpoints", "host",
                        "127.0.0.1", "apiPort", d->network_observed.api_port, "gitHost",
                        "127.0.0.1", "gitPort", d->network_observed.git_port);
    if (!outcome && value && d->oidc_enabled) {
        json_t *endpoints = json_object_get(value, "endpoints");
        if (!d->count) {
            json_object_del(endpoints, "apiPort");
            json_object_del(endpoints, "gitPort");
            json_object_del(endpoints, "gitHost");
        }
        json_object_set_new(endpoints, "oidcIssuerPort", json_integer(d->oidc_ports.issuer));
        json_object_set_new(endpoints, "oidcBridgePort", json_integer(d->oidc_ports.bridge));
        json_object_set_new(endpoints, "identityPort", json_integer(d->oidc_ports.identity));
        json_object_set_new(endpoints, "identityUiPort", json_integer(d->oidc_ports.ui));
        json_object_set_new(endpoints, "mailPort", json_integer(d->oidc_ports.mail));
    }
    if (host_failure && value)
        json_object_set_new(value, "requirement",
                            json_string(gstd_host_requirement_name((gstd_host_requirement)detail)));
    char *bytes = value ? gstd_json_dump(value) : NULL;
    json_decref(value);
    s = ok && bytes ? prepare(d, "UPDATE operations SET "
                                 "state=?,result=?,error=?,recovery=CASE WHEN ?=-2 THEN "
                                 "'uncertain' ELSE recovery "
                                 "END,updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') "
                                 "WHERE operation_id=? AND state IN ('accepted','running')")
                    : NULL;
    ok = ok && s;
    if (ok) {
        bind(s, 1, outcome ? (d->cancel_requested ? "cancelled" : "failed") : "succeeded");
        if (outcome) {
            sqlite3_bind_null(s, 2);
            bind(s, 3, bytes);
        } else {
            bind(s, 2, bytes);
            sqlite3_bind_null(s, 3);
        }
        sqlite3_bind_int(s, 4, outcome);
        bind(s, 5, d->target.operation_id);
        ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(d->environment->database) == 1;
    }
    sqlite3_finalize(s);
    free(bytes);
    if (commit(d, !ok)) {
        d->outcome = -2;
        return 0;
    }
    d->finalizing = 0;
    if (!outcome)
        d->ready_at_ms = now_ms();
    return 1;
}
static gstd_filesystem_input fs_input(gstd_deployment *d, gstd_filesystem_action action, int fd,
                                      int source) {
    return (gstd_filesystem_input){.action = action,
                                   .manifest = d->target.manifest,
                                   .store_id = d->target.store_id,
                                   .operation_id = d->target.operation_id,
                                   .deployment_id = d->target.deployment_id,
                                   .generation = d->target.generation,
                                   .descriptors = {fd, source, -1},
                                   .download_limit = UINT64_C(128) * 1024 * 1024,
                                   .merge_limit = UINT64_C(512) * 1024 * 1024,
                                   .image_timeout_ms = 180000};
}
static int scope_launch(gstd_deployment *d, const char *id, const char *kind, int stage) {
    if (sql(d, "BEGIN IMMEDIATE"))
        return finish(d, -2, 0);
    if (commit(d, gstd_deployment_resource_reserve(d->environment->database, &d->target, id, kind)))
        return finish(d, -2, 0);
    gstd_filesystem_input input = fs_input(d, GSTD_FS_SCOPE, d->environment->resource_fd, -1);
    input.resource_id = id;
    input.resource_kind = kind;
    d->stage = stage;
    if (gstd_filesystem_launch(&input, 10000, &d->filesystem))
        return finish(d, -1, 0);
    return 0;
}
static int record_scope(gstd_deployment *d, const char *id, const gstd_filesystem_reply *reply) {
    if (sql(d, "BEGIN IMMEDIATE"))
        return -1;
    return commit(d, gstd_deployment_scope_record(d->environment->database, &d->target, id, reply));
}
static int advance(gstd_deployment *d, int result, const char *uid) {
    gstd_bootstrap_graph before = d->graph;
    if (sql(d, "BEGIN IMMEDIATE"))
        return -1;
    if (commit(d,
               gstd_deployment_graph_step(d->environment->database, &d->target, d->configuration_id,
                                          &d->graph, d->graph.stage, result, uid))) {
        d->graph = before;
        return -1;
    }
    if (d->persistent)
        d->datastore.graph = d->graph;
    return 0;
}
static int boot_launch(gstd_deployment *d, gstd_bootstrap_job_kind kind,
                       gstd_bootstrap_http_action action) {
    gstd_bootstrap_job_input input = {
        .kind = kind,
        .preparation = {.manifest = d->target.manifest,
                        .store_id = d->target.store_id,
                        .deployment_id = d->target.deployment_id,
                        .operation_id = d->target.operation_id,
                        .datastore = d->persistent ? "scylladb" : "memdb",
                        .generation = d->target.generation,
                        .control_owner = getpid(),
                        .scope_fd = d->configuration.descriptors[1]},
        .broker_fd = d->environment->broker_fd,
        .source = d->credential_source,
        .allow_initial = d->allow_initial,
        .action = action,
        .network = &d->network_observed.network,
        .uid = d->graph.controller_uid,
        .api_boot = d->boot,
        .api_pid = d->workloads[1].native.expected_pid,
        .api_start = d->workloads[1].native.expected_start_time};
    if (d->oidc_enabled) {
        input.preparation.oidc_issuer_port = d->oidc_ports.issuer;
        input.preparation.oidc_client = &d->oidc.client;
    }
    return gstd_bootstrap_job_launch(&input, 20000, &d->bootstrap);
}
static int role(gstd_deployment *d, const char *name) {
    int fd = openat(d->configuration.descriptors[1], name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd >= 3 && (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
                    (st.st_mode & 07777) != 0600 || st.st_nlink != 1)) {
        close(fd);
        return -1;
    }
    return fd;
}
static int native_record(gstd_deployment *d, gstd_deployment_workload *w, const char *stage) {
    if (sql(d, "BEGIN IMMEDIATE"))
        return -1;
    sqlite3_stmt *s = prepare(d, "SELECT backend_identity FROM resource_ownership WHERE "
                                 "resource_id=? AND operation_id=? AND ownership_state='held'");
    json_t *value = NULL;
    if (s) {
        bind(s, 1, w->resource);
        bind(s, 2, d->target.operation_id);
        if (sqlite3_step(s) == SQLITE_ROW)
            value = gstd_json_parse(sqlite3_column_text(s, 0), (size_t)sqlite3_column_bytes(s, 0));
    }
    sqlite3_finalize(s);
    json_t *record =
        json_pack("{s:s,s:s,s:s,s:I,s:I,s:s,s:I}", "kind", "libcrun", "id", w->resource, "bootId",
                  d->boot, "pid", (json_int_t)w->native.expected_pid, "startTime",
                  (json_int_t)w->native.expected_start_time, "component", w->name, "generation",
                  (json_int_t)d->target.generation);
    int ok = value && record && !json_object_set(value, "native", record);
    json_t *population = w->native.expected_pid
                             ? gstd_native_population_identity(&w->population, w->resource, d->boot)
                             : NULL;
    json_t *prior = json_object_get(value, "population");
    if (w->native.expected_pid)
        ok = ok && population && (!prior || json_equal(prior, population)) &&
             !json_object_set(value, "population", population);
    json_decref(population);
    json_decref(record);
    char *bytes = ok ? gstd_json_dump(value) : NULL;
    json_decref(value);
    s = bytes ? prepare(d, "UPDATE resource_ownership SET "
                           "backend_identity=?,lifecycle_stage=?,observed_at="
                           "strftime('%Y-%m-%dT%H:%M:%"
                           "SZ','now') WHERE resource_id=? AND operation_id=? "
                           "AND ownership_state='held'")
              : NULL;
    ok = s != NULL;
    if (ok) {
        bind(s, 1, bytes);
        bind(s, 2, stage);
        bind(s, 3, w->resource);
        bind(s, 4, d->target.operation_id);
        ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(d->environment->database) == 1;
    }
    sqlite3_finalize(s);
    free(bytes);
    if (ok && !strcmp(stage, "running")) {
        s = prepare(d, "UPDATE components SET "
                       "backend_id=?,observed_state='running',observed_at=strftime('%Y-%m-%dT%"
                       "H:%M:"
                       "%SZ','now') WHERE deployment_id=? AND component=? AND generation=?");
        ok = s != NULL;
        if (ok) {
            bind(s, 1, w->resource);
            bind(s, 2, d->target.deployment_id);
            bind(s, 3, w->name);
            sqlite3_bind_int64(s, 4, (sqlite3_int64)d->target.generation);
            ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(d->environment->database) == 1;
        }
        sqlite3_finalize(s);
    }
    return commit(d, !ok);
}
static int native_launch(gstd_deployment *d, unsigned index) {
    d->index = index;
    gstd_deployment_workload *w = &d->workloads[index];
    w->inputs = (gstd_execution_inputs){.manifest = d->target.manifest,
                                        .component = w->name,
                                        .rootfs_fd = w->rootfs_fd,
                                        .network = &d->network_observed.network};
    for (unsigned i = 0; i < GSTD_MOUNT_COUNT; i++)
        w->inputs.role_fds[i] = -1;
    w->inputs.role_fds[GSTD_MOUNT_RESOLVER] = d->resolver_fd;
    if (index == 0) {
        w->inputs.role_fds[GSTD_MOUNT_BASE] = role(d, "git.toml");
        w->inputs.role_fds[GSTD_MOUNT_REPOSITORIES] = d->volume.data_fd;
    } else if (index == 1) {
        w->inputs.role_fds[GSTD_MOUNT_BASE] = role(d, "api-base.toml");
        w->inputs.role_fds[GSTD_MOUNT_OVERLAY] = role(d, "api-owned.toml");
        w->inputs.role_fds[GSTD_MOUNT_POLICY] = role(d, "policy.yaml");
        w->inputs.role_fds[GSTD_MOUNT_USERS] = role(d, "users.yaml");
    } else {
        w->inputs.role_fds[GSTD_MOUNT_BASE] = role(d, "controller-base.toml");
        w->inputs.role_fds[GSTD_MOUNT_OVERLAY] = role(d, "controller-owned.toml");
        w->inputs.role_fds[GSTD_MOUNT_SIGNING_RECORD] = role(d, "controller-signing.json");
    }
    w->native = (gstd_native_context){.backend_id = w->resource,
                                      .component = w->name,
                                      .boot_id = d->boot,
                                      .state_fd = d->environment->native_fd,
                                      .bundle_fd = w->scope.descriptors[2],
                                      .rootfs_fd = w->rootfs_fd,
                                      .writer_lease_fd = index == 0 ? d->volume.writer_fd : -1,
                                      .stdout_fd = d->capture.output[index][0],
                                      .stderr_fd = d->capture.output[index][1],
                                      .control_owner = getpid(),
                                      .registered_inputs = &w->inputs};
    w->native.configuration = gstd_execution_build(&w->inputs);
    if (!w->native.configuration || native_record(d, w, "allocated"))
        return finish(d, -1, 0);
    d->stage = NATIVE_CREATE;
    if (gstd_native_launch(&w->native, GSTD_NATIVE_CREATE, 20000, &d->native_job))
        return finish(d, -1, 0);
    return 0;
}
static int health_launch(gstd_deployment *d) {
    if (d->persistent && !d->graph.fresh_datastore && d->graph.stage == GSTD_BOOT_ENROLL &&
        d->graph.controller_uid[0]) {
        /* Reuse only the previously successful enrollment on this canonical
         * retained datastore. The controller must still authenticate and pass
         * readiness against the new API instance before start can succeed. */
        if (advance(d, 0, d->graph.controller_uid))
            return finish(d, -2, 0);
        d->stage = BIND;
        return boot_launch(d, GSTD_BOOT_JOB_BIND, GSTD_BOOT_API_READY) ? finish(d, -1, 0) : 0;
    }
    gstd_bootstrap_http_action action = d->graph.stage == GSTD_BOOT_WAIT_API   ? GSTD_BOOT_API_READY
                                        : d->graph.stage == GSTD_BOOT_WAIT_GIT ? GSTD_BOOT_GIT_READY
                                        : d->graph.stage == GSTD_BOOT_ENROLL
                                            ? GSTD_BOOT_CONTROLLER_ENROLL
                                            : GSTD_BOOT_CONTROLLER_READY;
    d->stage = HEALTH;
    if (boot_launch(d, GSTD_BOOT_JOB_HTTP, action))
        return finish(d, -1, 0);
    return 0;
}
int gstd_deployment_begin(const gstd_deployment_environment *environment, const char *operation,
                          gstd_deployment *d) {
    if (!environment || !environment->database || !environment->settings || !d || d->stage ||
        environment->cache_fd < 3 || environment->resource_fd < 3 || environment->native_fd < 3 ||
        environment->broker_fd < 3 || environment->state_fd < 3 ||
        !sqlite3_get_autocommit(environment->database))
        return -1;
    *d = (gstd_deployment){.environment = environment,
                           .volume = {.data_fd = -1, .writer_fd = -1},
                           .capture = GSTD_LOG_CAPTURE_INITIAL,
                           .resolver_fd = -1,
                           .keeper_death_fd = -1,
                           .helper_output = {-1, -1},
                           .network = GSTD_NETWORK_WORKER_INITIAL,
                           .network_observed = GSTD_NETWORK_OBSERVATION_INITIAL};
    if (gstd_deployment_target_read(environment->database, operation, &d->target) ||
        gstd_native_boot_id(d->boot))
        return -1;
    d->persistent = json_object_get(d->target.manifest, "datastore") != NULL;
    if (identifier(d->configuration_id, "cfg", operation) ||
        identifier(d->network_id, "net", operation))
        return -1;
    gstd_workload_role layout[GSTD_ROLE_COUNT];
    unsigned total = 0;
    if (gstd_workload_layout(d->target.manifest, layout, &total))
        return -1;
    d->oidc_enabled = json_object_get(d->target.manifest, "oidcProfileDigest") != NULL;
    for (unsigned i = 0; i < total; i++)
        if (layout[i] >= GSTD_ROLE_GIT)
            d->roles[d->count++] = layout[i];
    if ((d->count != 2 && d->count != 3) && !(d->oidc_enabled && !d->count))
        return -1;
    d->core_count = d->count;
    const char *names[GSTD_ROLE_COUNT];
    for (unsigned i = 0; i < d->count; i++)
        names[i] = gstd_workload_name(d->roles[i]);
    for (unsigned i = 0; i < d->count; i++) {
        gstd_deployment_workload *w = &d->workloads[i];
        w->name = names[i];
        w->rootfs_fd = -1;
        w->death_fd = -1;
        w->population = (gstd_native_population)GSTD_NATIVE_POPULATION_INITIAL;
        char role_name[32];
        snprintf(role_name, sizeof(role_name), "run-%s", names[i]);
        if (identifier(w->resource, role_name, operation))
            return -1;
        snprintf(role_name, sizeof(role_name), "asset-%s", names[i]);
        if (identifier(w->asset_resource, role_name, operation))
            return -1;
    }
    if (d->count) {
        sqlite3_stmt *s =
            prepare(d, "SELECT resource_id FROM resource_ownership WHERE operation_id=? AND "
                       "resource_type='repository-volume' AND ownership_state='held'");
        int ok = s != NULL;
        if (ok) {
            bind(s, 1, operation);
            ok = sqlite3_step(s) == SQLITE_ROW;
            if (ok) {
                const char *id = (const char *)sqlite3_column_text(s, 0);
                ok = id && strlen(id) < 65;
                if (ok)
                    memcpy(d->volume_id, id, strlen(id) + 1);
            }
        }
        sqlite3_finalize(s);
        if (!ok)
            return -1;
        d->credential_source = gstd_runtime_settings_source(
            environment->settings, d->target.store_id, d->target.deployment_id);
        if (!d->credential_source)
            return -1;
    }
    if (sql(d, "BEGIN IMMEDIATE"))
        return -1;
    json_t *images = json_object_get(d->target.manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        json_t *image = json_array_get(images, i);
        /* Datastore is an implicit owned dependency, not an application
         * component selector. Its native state lives in its resource receipt. */
        if (d->persistent &&
            !strcmp(json_string_value(json_object_get(image, "component")), "scylladb"))
            continue;
        sqlite3_stmt *component =
            prepare(d, "INSERT INTO components VALUES(?,?,NULL,?,'running','pending',?,"
                       "strftime('%Y-%m-%dT%H:%M:%SZ','now')) ON "
                       "CONFLICT(deployment_id,component) "
                       "DO UPDATE SET backend_id=NULL,image_digest=excluded.image_digest,"
                       "desired_state='running',observed_state='pending',generation="
                       "excluded.generation,"
                       "observed_at=excluded.observed_at");
        int inserted = component != NULL;
        if (inserted) {
            bind(component, 1, d->target.deployment_id);
            bind(component, 2, json_string_value(json_object_get(image, "component")));
            bind(component, 3, json_string_value(json_object_get(image, "digest")));
            sqlite3_bind_int64(component, 4, (sqlite3_int64)d->target.generation);
            inserted = sqlite3_step(component) == SQLITE_DONE;
        }
        sqlite3_finalize(component);
        if (!inserted) {
            sql(d, "ROLLBACK");
            return -1;
        }
    }
    if (commit(d, 0))
        return -1;
    d->deadline_ms = now_ms() + 600000;
    gstd_filesystem_input host = fs_input(d, GSTD_FS_HOST_PROBE, environment->resource_fd, -1);
    d->stage = HOST_PREFLIGHT;
    if (gstd_filesystem_launch(&host, 10000, &d->filesystem))
        finish(d, -1, GSTD_HOST_LIBRARY);
    return 0;
}
static int capture_record(gstd_deployment *d) {
    json_t *capture = gstd_log_capture_identity(&d->capture);
    if (!capture || sql(d, "BEGIN IMMEDIATE")) {
        json_decref(capture);
        return -1;
    }
    sqlite3_stmt *s = prepare(d, "SELECT backend_identity FROM resource_ownership WHERE "
                                 "resource_id=? AND operation_id=? "
                                 "AND deployment_id=? AND resource_type='configuration' AND "
                                 "ownership_state='held'");
    json_t *record = NULL;
    if (s) {
        bind(s, 1, d->configuration_id);
        bind(s, 2, d->target.operation_id);
        bind(s, 3, d->target.deployment_id);
        if (sqlite3_step(s) == SQLITE_ROW)
            record = gstd_json_parse(sqlite3_column_text(s, 0), (size_t)sqlite3_column_bytes(s, 0));
    }
    sqlite3_finalize(s);
    int valid = record && !json_object_get(record, "capture") &&
                !json_object_set(record, "capture", capture);
    char *bytes = valid ? gstd_json_dump(record) : NULL;
    json_decref(record);
    json_decref(capture);
    s = bytes ? prepare(d, "UPDATE resource_ownership SET "
                           "backend_identity=?,observed_at=strftime('%Y-%m-%dT%H:"
                           "%M:%SZ','now') WHERE "
                           "resource_id=? AND operation_id=? AND deployment_id=? "
                           "AND ownership_state='held'")
              : NULL;
    valid = s != NULL;
    if (valid) {
        bind(s, 1, bytes);
        bind(s, 2, d->configuration_id);
        bind(s, 3, d->target.operation_id);
        bind(s, 4, d->target.deployment_id);
        valid = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(d->environment->database) == 1;
    }
    sqlite3_finalize(s);
    free(bytes);
    return commit(d, !valid);
}
static int capture_launch(gstd_deployment *d) {
    gstd_log_capture_input input = {.state_fd = d->environment->state_fd,
                                    .configuration_fd = d->configuration.descriptors[0],
                                    .broker_fd = d->environment->broker_fd,
                                    .source = d->credential_source,
                                    .store_id = d->target.store_id,
                                    .deployment_id = d->target.deployment_id,
                                    .operation_id = d->target.operation_id,
                                    .generation = d->target.generation,
                                    .components = d->count + (unsigned)d->persistent +
                                                  (d->oidc_enabled ? GSTD_OIDC_SERVICE_COUNT : 0),
                                    .oidc = (unsigned)d->oidc_enabled,
                                    .oidc_source = d->oidc_enabled ? d->oidc.setup.source : NULL,
                                    .scylladb = (unsigned)d->persistent};
    d->stage = CAPTURE_READY;
    return gstd_log_capture_launch(&input, &d->capture) ? finish(d, -1, 0) : 0;
}
static int datastore_launch(gstd_deployment *d) {
    for (unsigned i = 0; i < 2; i++) {
        d->helper_output[i] = fcntl(d->capture.output[d->count][i], F_DUPFD_CLOEXEC, 3);
        if (d->helper_output[i] < 3)
            return finish(d, -1, 0);
    }
    d->stage = DATASTORE_SERVICE;
    if (gstd_datastore_service_begin(&d->datastore, &d->network_observed.network, d->boot,
                                     d->resolver_fd, d->capture.output[d->count][0],
                                     d->capture.output[d->count][1]))
        return finish(d, -1, 0);
    return 0;
}
static int datastore_helper_launch(gstd_deployment *d) {
    gstd_execution_purpose purpose;
    if (d->graph.stage == GSTD_BOOT_WAIT_DATASTORE)
        purpose = GSTD_EXEC_SCYLLA_READY;
    else if (d->graph.stage == GSTD_BOOT_PREPARE_KEYSPACE)
        purpose = GSTD_EXEC_SCYLLA_KEYSPACE;
    else if (d->graph.stage == GSTD_BOOT_MIGRATE_SCHEMA)
        purpose = GSTD_EXEC_SCHEMA_MIGRATE;
    else
        return finish(d, -2, 0);
    d->stage = DATASTORE_HELPER;
    if (gstd_datastore_helper_begin(&d->datastore, purpose, &d->network_observed.network, d->boot,
                                    d->resolver_fd, d->helper_output[0], d->helper_output[1]))
        return finish(d, -1, 0);
    return 0;
}
static int network_launch(gstd_deployment *d) {
    gstd_network_worker_request request = {.manifest = d->target.manifest,
                                           .store_id = d->target.store_id,
                                           .resource_id = d->network_id,
                                           .operation_id = d->target.operation_id,
                                           .scope_fd = d->network_scope.descriptors[1],
                                           .oidc_ports = d->oidc_enabled ? &d->oidc_ports : NULL};
    if (gstd_network_worker_prepare(&request, &d->network) != GSTD_NETWORK_OK)
        return finish(d, -1, 0);
    if (sql(d, "BEGIN IMMEDIATE"))
        return finish(d, -2, 0);
    if (commit(d, gstd_network_record_bind(d->environment->database, &request, &d->network)) ||
        gstd_network_worker_permit(&d->network))
        return finish(d, -2, 0);
    d->deadline_ms = now_ms() + 10000;
    d->stage = NETWORK_READY;
    return 0;
}
static int core_bootstrap(gstd_deployment *d) {
    if (sql(d, "BEGIN IMMEDIATE"))
        return finish(d, -2, 0);
    int failure = d->persistent
                      ? gstd_deployment_graph_matches(d->environment->database, &d->target,
                                                      d->configuration_id, &d->graph)
                      : gstd_deployment_graph_begin(d->environment->database, &d->target,
                                                    d->configuration_id, &d->graph);
    if (!failure)
        failure = gstd_deployment_credential_reserve(d->environment->database, &d->target,
                                                     d->configuration_id, &d->allow_initial);
    if (commit(d, failure))
        return finish(d, -2, 0);
    d->stage = BOOTSTRAP;
    return boot_launch(d, GSTD_BOOT_JOB_PREPARE, GSTD_BOOT_API_READY) ? finish(d, -1, 0) : 0;
}
int gstd_deployment_poll(gstd_deployment *d, int cancel) {
    if (!d || !d->stage || !sqlite3_get_autocommit(d->environment->database))
        return -1;
    if (d->stage == FINISHED)
        return d->finalizing ? finish(d, d->final_outcome, d->final_detail) : 1;
    if (d->stage == CAPTURE_READY) {
        int ready = gstd_log_capture_poll(&d->capture, cancel);
        if (ready <= 0)
            return ready;
        if (d->capture.status || capture_record(d))
            return finish(d, -1, 0);
        if (d->oidc_enabled) {
            d->stage = OIDC_EXECUTE;
            return gstd_oidc_runtime_execute_begin(
                       &d->oidc, &d->network_observed.network, &d->network, d->boot, d->resolver_fd,
                       (const int(*)[2]) & d->capture.output[d->count + (unsigned)d->persistent])
                       ? finish(d, -2, 0)
                       : 0;
        }
        return d->persistent ? datastore_launch(d) : native_launch(d, 0);
    }
    if (d->stage == OIDC_FINAL_HEALTH) {
        int ready = gstd_oidc_runtime_execute_poll(&d->oidc, cancel);
        return ready == 1 ? finish(d, d->oidc.execute_outcome, d->oidc.detail) : ready;
    }
    if (d->stage == OIDC_PREPARE) {
        int ready = gstd_oidc_runtime_prepare_poll(&d->oidc, cancel);
        if (ready != 1)
            return ready;
        if (d->oidc.outcome)
            return finish(d, d->oidc.outcome, d->oidc.detail);
        if (sql(d, "BEGIN IMMEDIATE"))
            return finish(d, -2, 0);
        int failed = 0;
        for (unsigned i = 0; !failed && i < GSTD_OIDC_WORKLOAD_COUNT; i++)
            failed = gstd_network_attachment_reserve(d->environment->database, &d->network,
                                                     &d->network_observed,
                                                     d->oidc.workloads[i].resource);
        if (commit(d, failed))
            return finish(d, -2, 0);
        return d->count ? core_bootstrap(d) : capture_launch(d);
    }
    if (d->stage == OIDC_EXECUTE) {
        int ready = gstd_oidc_runtime_execute_poll(&d->oidc, cancel);
        for (unsigned i = 0; i < GSTD_OIDC_SERVICE_COUNT; i++)
            if (d->oidc.workloads[i].launched)
                gstd_log_capture_output_close(&d->capture, d->count + (unsigned)d->persistent + i);
        if (ready != 1)
            return ready;
        if (d->oidc.execute_outcome)
            return finish(d, d->oidc.execute_outcome, d->oidc.detail);
        return d->persistent ? datastore_launch(d)
               : d->count    ? native_launch(d, 0)
                             : finish(d, 0, 0);
    }
    if (d->stage == DATASTORE_PREPARE) {
        int ready = gstd_datastore_prepare_poll(&d->datastore, cancel);
        if (ready != 1)
            return ready;
        if (d->datastore.outcome)
            return finish(d, d->datastore.outcome, d->datastore.detail);
        d->graph = d->datastore.graph;
        return scope_launch(d, d->workloads[0].resource, "workload", WORK_SCOPE);
    }
    if (d->cancel_requested && cancel >= 3 && !d->filesystem.worker && !d->native_job.worker &&
        !d->bootstrap.worker && !d->asset.worker.worker && !d->datastore.service_job.worker &&
        !d->datastore.helper.stage) {
        struct pollfd notification = {.fd = cancel, .events = POLLIN};
        if (poll(&notification, 1, 0) > 0 && notification.revents)
            return finish(d, -1, 0);
    }
    gstd_filesystem_reply reply = {0};
    int ready;
    sqlite3 *db = d->environment->database;
    if (d->stage == DATASTORE_SERVICE) {
        ready = gstd_datastore_service_poll(&d->datastore, cancel);
        gstd_deployment_workload *service = &d->datastore.workloads[0];
        if (service->native.expected_pid > 0) {
            gstd_log_capture_output_close(&d->capture, d->count);
            service->native.stdout_fd = service->native.stderr_fd = -1;
        }
        if (ready != 1)
            return ready;
        if (d->datastore.service_outcome)
            return finish(d, d->datastore.service_outcome, 0);
        d->graph = d->datastore.graph;
        d->deadline_ms = now_ms() + 180000;
        return d->cancel_requested ? finish(d, -1, 0) : datastore_helper_launch(d);
    }
    if (d->stage == DATASTORE_HELPER) {
        if (!d->datastore.helper.stage) {
            if (now_ms() < d->next_health_ms)
                return 0;
            return d->cancel_requested ? finish(d, -1, 0) : datastore_helper_launch(d);
        }
        ready = gstd_datastore_helper_poll(&d->datastore, cancel);
        if (ready != 1)
            return ready;
        int outcome = d->datastore.helper.outcome;
        int pending = outcome == -1 && d->graph.stage == GSTD_BOOT_WAIT_DATASTORE;
        if (gstd_datastore_graph_step(&d->datastore, d->graph.stage, pending ? 1 : outcome))
            return finish(d, -2, 0);
        d->graph = d->datastore.graph;
        if (outcome == -2)
            return finish(d, -2, 0);
        if (gstd_datastore_helper_close(&d->datastore))
            return finish(d, -2, 0);
        if (d->cancel_requested || (outcome && !pending))
            return finish(d, -1, 0);
        if (pending) {
            if (now_ms() >= d->deadline_ms)
                return finish(d, -1, 0);
            d->next_health_ms = now_ms() + 250;
            return 0;
        }
        if (d->graph.stage == GSTD_BOOT_LAUNCH_GIT) {
            for (unsigned i = 0; i < 2; i++) {
                close(d->helper_output[i]);
                d->helper_output[i] = -1;
            }
            return native_launch(d, 0);
        }
        return datastore_helper_launch(d);
    }
    if (d->stage == ASSET) {
        ready = gstd_deployment_asset_poll(&d->asset, cancel);
        if (ready < 0)
            return -1;
        if (!ready)
            return 0;
        if (d->asset.outcome)
            return finish(d, d->asset.outcome, d->asset.detail);
        gstd_deployment_workload *w = &d->workloads[d->index];
        gstd_filesystem_input input =
            fs_input(d, GSTD_FS_IMAGE_MATERIALIZE, w->scope.descriptors[1], d->asset.active_fd);
        input.component = w->name;
        d->stage = MATERIALIZE;
        if (gstd_filesystem_launch(&input, 190000, &d->filesystem))
            return finish(d, -1, 0);
        return 0;
    }
    if (d->stage == NATIVE_CREATE || d->stage == NATIVE_START || d->stage == NATIVE_OBSERVE) {
        gstd_deployment_workload *w = &d->workloads[d->index];
        gstd_native_status status;
        ready = gstd_native_poll(&d->native_job, cancel, &status, &w->observed);
        if (ready < 0)
            return -1;
        if (!ready)
            return 0;
        if (d->stage == NATIVE_CREATE) {
            gstd_log_capture_output_close(&d->capture, d->index);
            w->native.stdout_fd = w->native.stderr_fd = -1;
        }
        if (status != GSTD_NATIVE_OK)
            return finish(d, status == GSTD_NATIVE_UNCERTAIN ? -2 : -1, (int)status);
        if (d->stage == NATIVE_CREATE) {
            w->native.expected_pid = w->observed.pid;
            w->native.expected_start_time = w->observed.start_time;
            w->launched = 1;
            if (gstd_native_population_open(w->resource, w->native.expected_pid,
                                            w->native.expected_start_time, d->boot,
                                            &w->population) ||
                gstd_native_population_empty(&w->population) != 0 ||
                gstd_native_watch_open(w->native.expected_pid, w->native.expected_start_time,
                                       d->boot, &w->death_fd) ||
                native_record(d, w, "allocated"))
                return finish(d, -2, 0);
            d->stage = NATIVE_START;
            if (gstd_native_launch(&w->native, GSTD_NATIVE_START, 10000, &d->native_job))
                return finish(d, -2, 0);
            return 0;
        }
        if (d->stage == NATIVE_START) {
            d->stage = NATIVE_OBSERVE;
            if (gstd_native_launch(&w->native, GSTD_NATIVE_OBSERVE, 10000, &d->native_job))
                return finish(d, -2, 0);
            return 0;
        }
        if (!w->observed.running ||
            gstd_native_population_matches(&w->population, w->resource, w->native.expected_pid,
                                           w->native.expected_start_time, d->boot) ||
            native_record(d, w, "running") || advance(d, 0, NULL))
            return finish(d, -2, 0);
        if (d->index == 0) {
            close(d->volume.writer_fd);
            d->volume.writer_fd = -1;
            w->native.writer_lease_fd = -1;
            return native_launch(d, 1);
        }
        d->deadline_ms = now_ms() + 120000;
        return health_launch(d);
    }
    if (d->stage == BOOTSTRAP || d->stage == BIND || d->stage == HEALTH) {
        if (!d->bootstrap.worker) {
            if (now_ms() < d->next_health_ms)
                return 0;
            return health_launch(d);
        }
        gstd_bootstrap_job_reply observed = {0};
        ready = gstd_bootstrap_job_poll(&d->bootstrap, cancel, &observed);
        if (ready < 0)
            return -1;
        if (!ready)
            return 0;
        if (advance(d, observed.status, *observed.uid ? observed.uid : NULL))
            return finish(d, -2, 0);
        if (observed.status < 0)
            return finish(d, observed.status, 0);
        if (d->stage == BOOTSTRAP)
            return capture_launch(d);
        if (d->stage == BIND)
            return native_launch(d, 2);
        if (observed.status == GSTD_BOOT_HTTP_PENDING) {
            if (now_ms() >= d->deadline_ms)
                return finish(d, -1, 0);
            d->next_health_ms = now_ms() + 250;
            return 0;
        }
        if (d->graph.stage == GSTD_BOOT_COMPLETE)
            return finish(d, 0, 0);
        if (d->graph.stage == GSTD_BOOT_BIND_CONTROLLER) {
            d->stage = BIND;
            if (boot_launch(d, GSTD_BOOT_JOB_BIND, GSTD_BOOT_API_READY))
                return finish(d, -1, 0);
            return 0;
        }
        return health_launch(d);
    }
    if (d->stage == NETWORK_READY) {
        ready = gstd_network_worker_ready(&d->network, &d->network_observed);
        if (!ready) {
            if (now_ms() >= d->deadline_ms)
                return finish(d, -2, 0);
            return 0;
        }
        if (ready < 0 || sql(d, "BEGIN IMMEDIATE"))
            return finish(d, -2, 0);
        int failure = gstd_network_record_running(db, &d->network, &d->network_observed);
        for (unsigned i = 0; !failure && i < d->count; i++)
            failure = gstd_network_attachment_reserve(db, &d->network, &d->network_observed,
                                                      d->workloads[i].resource);
        for (unsigned i = 0; !failure && d->persistent && i < 3; i++)
            failure = gstd_network_attachment_reserve(db, &d->network, &d->network_observed,
                                                      d->datastore.workloads[i].resource);
        if (commit(d, failure) ||
            gstd_native_watch_open(d->network_observed.network.keeper_pid,
                                   d->network_observed.network.keeper_start,
                                   d->network_observed.network.boot_id, &d->keeper_death_fd))
            return finish(d, -2, 0);
        gstd_filesystem_input input =
            fs_input(d, GSTD_FS_RESOLVER, d->configuration.descriptors[0], -1);
        d->stage = RESOLVER;
        if (gstd_filesystem_launch(&input, 10000, &d->filesystem))
            return finish(d, -1, 0);
        return 0;
    }
    ready = gstd_filesystem_poll(&d->filesystem, cancel, &reply);
    if (ready < 0)
        return -1;
    if (!ready)
        return 0;
    if (reply.status) {
        int outcome = reply.status == -2 ? -2 : -1, detail = reply.detail;
        gstd_filesystem_reply_close(&reply);
        return finish(d, outcome, detail);
    }
    if (d->stage == HOST_PREFLIGHT)
        return scope_launch(d, d->configuration_id, "configuration", CONFIG_SCOPE);
    if (d->stage == CONFIG_SCOPE) {
        d->configuration = reply;
        if (record_scope(d, d->configuration_id, &reply))
            return finish(d, -2, 0);
        if (d->persistent) {
            d->stage = DATASTORE_PREPARE;
            return gstd_datastore_prepare_begin(d->environment, &d->target, d->configuration_id,
                                                &d->datastore)
                       ? finish(d, -1, 0)
                       : 0;
        }
        return d->count ? scope_launch(d, d->workloads[0].resource, "workload", WORK_SCOPE)
                        : scope_launch(d, d->network_id, "network", NETWORK_SCOPE);
    }
    if (d->stage == WORK_SCOPE) {
        gstd_deployment_workload *w = &d->workloads[d->index];
        w->scope = reply;
        if (record_scope(d, w->resource, &reply) || sql(d, "BEGIN IMMEDIATE"))
            return finish(d, -2, 0);
        if (commit(d, gstd_deployment_resource_reserve(db, &d->target, w->asset_resource,
                                                       "asset-root")))
            return finish(d, -2, 0);
        int source = gstd_runtime_settings_image(d->environment->settings, w->name);
        d->stage = ASSET;
        if (gstd_deployment_asset_begin(db, &d->target, w->name, w->asset_resource,
                                        d->environment->cache_fd, source,
                                        UINT64_C(128) * 1024 * 1024, UINT64_C(512) * 1024 * 1024,
                                        UINT64_C(16) * 1024 * 1024 * 1024, &d->asset))
            return finish(d, -1, 0);
        return 0;
    }
    if (d->stage == MATERIALIZE) {
        if (reply.descriptor_count != 1) {
            gstd_filesystem_reply_close(&reply);
            return finish(d, -2, 0);
        }
        d->workloads[d->index].rootfs_fd = reply.descriptors[0];
        if (sql(d, "BEGIN IMMEDIATE"))
            return finish(d, -2, 0);
        gstd_deployment_workload *materialized = &d->workloads[d->index];
        if (commit(d, gstd_deployment_execution_record(d->environment->database, &d->target,
                                                       materialized->resource, &materialized->scope,
                                                       materialized->rootfs_fd)))
            return finish(d, -2, 0);
        if (gstd_deployment_asset_close(&d->asset))
            return finish(d, -2, 0);
        if (++d->index < d->count)
            return scope_launch(d, d->workloads[d->index].resource, "workload", WORK_SCOPE);
        if (sql(d, "BEGIN IMMEDIATE"))
            return finish(d, -2, 0);
        gstd_volume_identity expected;
        int existing = gstd_volume_record_begin(db, &d->environment->volume_root, d->volume_id,
                                                d->target.operation_id, d->producer_id, &expected);
        if (commit(d, existing < 0))
            return finish(d, -2, 0);
        gstd_filesystem_input input =
            fs_input(d, GSTD_FS_VOLUME, d->environment->volume_root.root_fd,
                     d->environment->volume_root.lock_fd);
        input.volume_root = d->environment->volume_root;
        input.volume_id = d->volume_id;
        input.producer_id = d->producer_id;
        input.volume_existing = existing;
        input.volume_expected = expected;
        d->stage = VOLUME;
        if (gstd_filesystem_launch(&input, 10000, &d->filesystem))
            return finish(d, -1, 0);
        return 0;
    }
    if (d->stage == VOLUME) {
        d->volume = (gstd_volume_handle){.data_fd = reply.descriptors[0],
                                         .writer_fd = reply.descriptors[1],
                                         .identity = reply.volume};
        if (sql(d, "BEGIN IMMEDIATE"))
            return finish(d, -2, 0);
        if (commit(d, gstd_volume_record_ready(db, &d->environment->volume_root, d->volume_id,
                                               d->target.operation_id, &d->volume)))
            return finish(d, -2, 0);
        return scope_launch(d, d->network_id, "network", NETWORK_SCOPE);
    }
    if (d->stage == NETWORK_SCOPE) {
        d->network_scope = reply;
        if (!d->oidc_enabled)
            return network_launch(d);
        if (sql(d, "BEGIN IMMEDIATE"))
            return finish(d, -2, 0);
        int selected = gstd_deployment_oidc_ports_read(db, &d->target, &d->oidc_ports);
        if (commit(d, selected < 0))
            return finish(d, -2, 0);
        if (!selected)
            return network_launch(d);
        gstd_filesystem_input request =
            fs_input(d, GSTD_FS_OIDC_PORTS, d->network_scope.descriptors[1], -1);
        request.resource_id = d->network_id;
        d->stage = OIDC_PORTS;
        return gstd_filesystem_launch(&request, 5000, &d->filesystem) ? finish(d, -1, 0) : 0;
    }
    if (d->stage == OIDC_PORTS) {
        if (gstd_oidc_ports_check(&reply.oidc_ports))
            return finish(d, -2, 0);
        d->oidc_ports = reply.oidc_ports;
        return network_launch(d);
    }
    if (d->stage == RESOLVER) {
        d->resolver_fd = reply.descriptors[0];
        if (!d->oidc_enabled)
            return core_bootstrap(d);
        d->stage = OIDC_PREPARE;
        return gstd_oidc_runtime_prepare_begin(d->environment, &d->target, &d->oidc_ports, &d->oidc)
                   ? finish(d, -2, 0)
                   : 0;
    }
    gstd_filesystem_reply_close(&reply);
    return finish(d, -2, 0);
}

int gstd_deployment_recovery_prepare(const gstd_deployment_environment *environment,
                                     const char *operation, gstd_deployment *d) {
    if (!environment || !environment->database || !environment->settings || !d || d->stage ||
        environment->cache_fd < 3 || environment->resource_fd < 3 || environment->native_fd < 3 ||
        !sqlite3_get_autocommit(environment->database))
        return -1;
    *d = (gstd_deployment){.environment = environment,
                           .volume = {.data_fd = -1, .writer_fd = -1},
                           .capture = GSTD_LOG_CAPTURE_INITIAL,
                           .resolver_fd = -1,
                           .keeper_death_fd = -1,
                           .helper_output = {-1, -1},
                           .network = GSTD_NETWORK_WORKER_INITIAL,
                           .network_observed = GSTD_NETWORK_OBSERVATION_INITIAL,
                           .recovered = 1,
                           .outcome = -1};
    if (gstd_deployment_target_recovery_read(environment->database, operation, &d->target) ||
        gstd_native_boot_id(d->boot) || identifier(d->configuration_id, "cfg", operation) ||
        identifier(d->network_id, "net", operation))
        return -1;
    d->persistent = json_object_get(d->target.manifest, "datastore") != NULL;
    if (d->persistent &&
        gstd_datastore_recovery_seed(environment, &d->target, d->configuration_id, &d->datastore))
        return -1;
    gstd_workload_role layout[GSTD_ROLE_COUNT];
    unsigned total = 0;
    if (gstd_workload_layout(d->target.manifest, layout, &total))
        return -1;
    d->oidc_enabled = json_object_get(d->target.manifest, "oidcProfileDigest") != NULL;
    for (unsigned i = 0; i < total; i++)
        if (layout[i] >= GSTD_ROLE_GIT)
            d->roles[d->count++] = layout[i];
    if ((d->count != 2 && d->count != 3) && !(d->oidc_enabled && !d->count))
        return -1;
    d->core_count = d->count;
    if (d->oidc_enabled && gstd_oidc_runtime_recovery_seed(environment, &d->target, &d->oidc))
        return -1;
    const char *names[GSTD_ROLE_COUNT];
    for (unsigned i = 0; i < d->count; i++)
        names[i] = gstd_workload_name(d->roles[i]);
    for (unsigned i = 0; i < d->count; i++) {
        gstd_deployment_workload *w = &d->workloads[i];
        w->name = names[i];
        w->rootfs_fd = -1;
        w->death_fd = -1;
        w->population = (gstd_native_population)GSTD_NATIVE_POPULATION_INITIAL;
        for (unsigned j = 0; j < GSTD_MOUNT_COUNT; j++)
            w->inputs.role_fds[j] = -1;
        char name[32];
        snprintf(name, sizeof(name), "run-%s", names[i]);
        if (identifier(w->resource, name, operation))
            return -1;
        snprintf(name, sizeof(name), "asset-%s", names[i]);
        if (identifier(w->asset_resource, name, operation))
            return -1;
    }
    sqlite3_stmt *s = prepare(d, "SELECT generation,last_operation_id FROM "
                                 "deployments WHERE deployment_id=?");
    if (!s)
        return -1;
    bind(s, 1, d->target.deployment_id);
    int valid = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_int64 generation = valid ? sqlite3_column_int64(s, 0) : 0;
    const char *last = valid ? (const char *)sqlite3_column_text(s, 1) : NULL;
    valid = valid && generation > 0 && generation < GSTD_SAFE_INTEGER && last &&
            gstd_identifier(last, (size_t)sqlite3_column_bytes(s, 1));
    if (valid) {
        d->recovery_generation = (uint64_t)generation;
        memcpy(d->recovery_operation, last, strlen(last) + 1);
    }
    sqlite3_finalize(s);
    if (!valid)
        return -1;
    d->stage = FINISHED;
    return 0;
}

#ifdef GITSTORED_TESTING
/* Controlled metadata fixture only. Does not construct/dispatch a backend or
 * grant recovery authority; production builds have no such entry point. */
int gstd_test_deployment_finish_failure(gstd_deployment *d) { return finish(d, -1, 0); }
#endif
