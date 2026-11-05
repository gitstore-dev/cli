#define _GNU_SOURCE 1
#include "datastore.h"
#include "gitstored/asset_records.h"
#include "gitstored/operations.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "migration.h"
#include "migration_history.h"
#include "migration_recovery.h"
#include "runtime_manager.h"
gstd_runtime_manager *gstd_test_runtime_reopened(const gstd_deployment_environment *);
int gstd_test_runtime_schema_observed(const gstd_runtime_manager *);
#include "migration_runtime.h"
#include "migration_schema.h"
#include "migration_stop.h"
#include "native_exit.h"
#include "native_recovery.h"
#include "network_records.h"
#include "recovery.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "migration preparation line %d\n", __LINE__);                          \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int resource_name(char output[65], const char *prefix, const char *operation) {
    int length = snprintf(output, 65, "%s%s", prefix, operation);
    return length > 0 && length < 65 ? 0 : -1;
}
static int wait_fs(gstd_filesystem_job *job, gstd_filesystem_reply *reply) {
    for (;;) {
        int ready = gstd_filesystem_poll(job, -1, reply);
        if (ready)
            return ready;
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
}
static void pause_poll(void) {
    struct timespec delay = {0, 10000000};
    nanosleep(&delay, NULL);
}
static int helper_paused(const gstd_helper *helper) {
    char path[64], bytes[4096];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)helper->context.expected_pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 3) {
        if (fd >= 0)
            close(fd);
        return 0;
    }
    ssize_t n = read(fd, bytes, sizeof(bytes) - 1);
    close(fd);
    if (n < 4 || n >= (ssize_t)sizeof(bytes) - 1)
        return 0;
    bytes[n] = 0;
    char *last = strrchr(bytes, ')');
    return last && last[1] == ' ' && (last[2] == 'T' || last[2] == 't') &&
           gstd_native_watch_poll(helper->death_fd) == 0;
}
static json_t *owned_identity(sqlite3 *db, const char *operation, const char *resource) {
    sqlite3_stmt *s = NULL;
    json_t *value = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT backend_identity FROM resource_ownership WHERE "
                           "operation_id=? AND resource_id=? AND ownership_state='held'",
                           -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, resource, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW)
            value = gstd_json_parse(sqlite3_column_text(s, 0), (size_t)sqlite3_column_bytes(s, 0));
    }
    sqlite3_finalize(s);
    return value;
}
static int reopen_scope(const gstd_deployment_environment *e, const gstd_deployment_target *t,
                        const char *id, const char *kind, gstd_filesystem_reply *reply) {
    json_t *identity = owned_identity(e->database, t->operation_id, id);
    if (!identity) {
        json_decref(identity);
        return -1;
    }
    gstd_filesystem_input input = {.action = GSTD_FS_REOPEN_SCOPE,
                                   .manifest = t->manifest,
                                   .scope_expected = identity,
                                   .store_id = t->store_id,
                                   .operation_id = t->operation_id,
                                   .deployment_id = t->deployment_id,
                                   .generation = t->generation,
                                   .resource_id = id,
                                   .resource_kind = kind,
                                   .descriptors = {e->resource_fd, -1, -1}};
    gstd_filesystem_job job = {0};
    int valid =
        !gstd_filesystem_launch(&input, 10000, &job) && wait_fs(&job, reply) == 1 && !reply->status;
    json_decref(identity);
    return valid ? 0 : -1;
}
static int service_check(gstd_datastore *d, const gstd_filesystem_reply *configuration, int resume,
                         int migrate) {
    sqlite3 *db = d->environment->database;
    char network_id[65], boot[37];
    CHECK(!resource_name(network_id, "net-", d->target->operation_id));
    CHECK(!gstd_native_boot_id(boot));
    if (!resume) {
        CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
        CHECK(!gstd_deployment_resource_reserve(db, d->target, network_id, "network"));
        CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    }
    gstd_filesystem_input input = {.action = GSTD_FS_SCOPE,
                                   .manifest = d->target->manifest,
                                   .store_id = d->target->store_id,
                                   .operation_id = d->target->operation_id,
                                   .deployment_id = d->target->deployment_id,
                                   .generation = d->target->generation,
                                   .resource_id = network_id,
                                   .resource_kind = "network",
                                   .descriptors = {d->environment->resource_fd, -1, -1}};
    gstd_filesystem_job job = {0};
    gstd_filesystem_reply scope = {0}, resolver = {0}, writer = {0};
    if (resume)
        CHECK(!reopen_scope(d->environment, d->target, network_id, "network", &scope));
    else
        CHECK(!gstd_filesystem_launch(&input, 10000, &job) && wait_fs(&job, &scope) == 1 &&
              !scope.status);
    gstd_network_worker network = GSTD_NETWORK_WORKER_INITIAL;
    gstd_network_worker_observation observed = GSTD_NETWORK_OBSERVATION_INITIAL;
    gstd_network_worker_request request = {.manifest = d->target->manifest,
                                           .store_id = d->target->store_id,
                                           .resource_id = network_id,
                                           .operation_id = d->target->operation_id,
                                           .scope_fd = scope.descriptors[resume ? 0 : 1]};
    int ready = 1;
    if (resume) {
        json_t *identity = owned_identity(db, d->target->operation_id, network_id);
        CHECK(identity && !gstd_network_worker_reopen(&request, identity, &network, &observed));
        json_decref(identity);
    } else {
        CHECK(gstd_network_worker_prepare(&request, &network) == GSTD_NETWORK_OK);
        CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
        CHECK(!gstd_network_record_bind(db, &request, &network));
        CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
        CHECK(!gstd_network_worker_permit(&network));
        do {
            ready = gstd_network_worker_ready(&network, &observed);
            if (!ready)
                pause_poll();
        } while (!ready);
    }
    CHECK(ready == 1);
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!gstd_network_record_running(db, &network, &observed));
    for (unsigned i = 0; i < 3; i++)
        CHECK(!gstd_network_attachment_reserve(db, &network, &observed, d->workloads[i].resource));
    CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    int keeper = -1;
    CHECK(!gstd_native_watch_open(observed.network.keeper_pid, observed.network.keeper_start,
                                  observed.network.boot_id, &keeper));
    input = (gstd_filesystem_input){.action = GSTD_FS_RESOLVER,
                                    .descriptors = {configuration->descriptors[0], -1, -1}};
    int existing_resolver = resume ? openat(configuration->descriptors[0], "resolver.conf",
                                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW)
                                   : -1;
    if (existing_resolver >= 3) {
        resolver.descriptor_count = 1;
        resolver.descriptors[0] = existing_resolver;
    } else
        CHECK(!gstd_filesystem_launch(&input, 10000, &job) && wait_fs(&job, &resolver) == 1 &&
              !resolver.status);
    json_t *source_identity =
        resume ? owned_identity(db, d->target->operation_id, d->workloads[0].resource) : NULL;
    if (source_identity && json_object_get(source_identity, "native")) {
        gstd_deployment_workload *w = &d->workloads[0];
        gstd_native_observation identity = {0};
        CHECK(gstd_native_recovery_read(source_identity, "running", w->resource, "scylladb", boot,
                                        d->target->generation,
                                        &identity) == GSTD_RECOVERY_NATIVE_KNOWN);
        memcpy(d->service_boot, boot, 37);
        w->native = (gstd_native_context){.backend_id = w->resource,
                                          .component = "scylladb",
                                          .boot_id = d->service_boot,
                                          .state_fd = d->environment->native_fd,
                                          .bundle_fd = w->scope.descriptors[2],
                                          .rootfs_fd = w->rootfs_fd,
                                          .stdout_fd = -1,
                                          .stderr_fd = -1,
                                          .writer_lease_fd = -1,
                                          .control_owner = getpid(),
                                          .expected_pid = identity.pid,
                                          .expected_start_time = identity.start_time};
        gstd_native_job observation = {0};
        CHECK(!gstd_native_launch(&w->native, GSTD_NATIVE_OBSERVE, 10000, &observation));
        gstd_native_status status;
        do {
            ready = gstd_native_poll(&observation, -1, &status, &w->observed);
            if (!ready)
                pause_poll();
        } while (!ready);
        CHECK(ready == 1 && status == GSTD_NATIVE_OK && w->observed.running &&
              w->observed.pid == identity.pid && w->observed.start_time == identity.start_time);
        CHECK(!gstd_native_watch_open(identity.pid, identity.start_time, boot, &w->death_fd));
        CHECK(!gstd_native_population_open(w->resource, identity.pid, identity.start_time, boot,
                                           &w->population));
        w->launched = 1;
        CHECK(!gstd_datastore_service_recovered(d, boot));
    } else {
        CHECK(!gstd_datastore_service_begin(d, &observed.network, boot, resolver.descriptors[0], -1,
                                            -1));
        do {
            ready = gstd_datastore_service_poll(d, -1);
            if (!ready)
                pause_poll();
        } while (!ready);
        CHECK(ready == 1 && !d->service_outcome);
    }
    json_decref(source_identity);
    if (migrate) {
        gstd_migration_schema schema = {0};
        CHECK(!gstd_migration_schema_begin(d, &observed.network, resolver.descriptors[0], &schema));
        do {
            ready = gstd_migration_schema_poll(&schema, -1);
            if (!ready)
                pause_poll();
        } while (!ready);
        CHECK(ready == 1 && !schema.outcome);
        CHECK(!gstd_migration_schema_close(&schema));
    } else {
        /* Every helper is observed/deleted before its output is sealed or reused. */
        int cql_ready = 0;
        for (unsigned attempt = 0; attempt < (resume ? 3u : 60u) && !cql_ready; attempt++) {
            int diagnostic = gstd_migration_output_create();
            CHECK(diagnostic >= 3);
            CHECK(!gstd_datastore_helper_begin(d, GSTD_EXEC_SCYLLA_READY, &observed.network, boot,
                                               resolver.descriptors[0], diagnostic, diagnostic));
            do {
                ready = gstd_datastore_helper_poll(d, -1);
                if (!ready)
                    pause_poll();
            } while (!ready);
            CHECK(ready == 1 && d->helper.outcome != -2);
            cql_ready = d->helper.outcome == 0;
            CHECK(!gstd_datastore_helper_close(d));
            if (!cql_ready && attempt < 3) {
                char bytes[2049];
                ssize_t n = pread(diagnostic, bytes, sizeof(bytes) - 1, 0);
                if (n > 0) {
                    bytes[n] = 0;
                    fprintf(stderr, "CQL readiness diagnostic: %s\n", bytes);
                }
            }
            close(diagnostic);
            if (!cql_ready) {
                struct timespec delay = {1, 0};
                nanosleep(&delay, NULL);
            }
        }
        CHECK(cql_ready);
        for (unsigned step = 0; step < 2; step++) {
            int output = gstd_migration_output_create();
            CHECK(output >= 3);
            CHECK(!gstd_datastore_helper_begin(
                d, step ? GSTD_EXEC_SCHEMA_HISTORY : GSTD_EXEC_SCHEMA_INSPECT, &observed.network,
                boot, resolver.descriptors[0], output, -1));
            do {
                ready = gstd_datastore_helper_poll(d, -1);
                if (!ready)
                    pause_poll();
            } while (!ready);
            CHECK(ready == 1 && !d->helper.outcome);
            CHECK(!gstd_datastore_helper_close(d));
            json_t *rows = gstd_migration_output_rows(output, step ? 1 : 2);
            CHECK(rows);
            if (step)
                CHECK(!gstd_migration_history_validate("gitstore-06152d-schema-012",
                                                       json_array_get(rows, 0), 1));
            else {
                int ledger = 0;
                CHECK(!gstd_migration_schema_inspection(rows, &ledger) && ledger);
            }
            json_decref(rows);
            close(output);
        }
    }
    gstd_migration_stop stop = {0};
    CHECK(!gstd_migration_stop_begin(d, &stop));
    do {
        ready = gstd_migration_stop_poll(&stop);
        if (!ready)
            pause_poll();
    } while (!ready);
    CHECK(ready == 1 && !stop.outcome && !d->workloads[0].launched);
    input = (gstd_filesystem_input){.action = GSTD_FS_DATASTORE_VOLUME,
                                    .manifest = d->target->manifest,
                                    .volume_root = d->environment->volume_root,
                                    .volume_id = d->volume_id,
                                    .producer_id = d->producer_id,
                                    .volume_existing = 1,
                                    .volume_expected = d->volume.identity,
                                    .descriptors = {d->environment->volume_root.root_fd,
                                                    d->environment->volume_root.lock_fd, -1}};
    CHECK(!gstd_filesystem_launch(&input, 5000, &job) && wait_fs(&job, &writer) == 1 &&
          !writer.status);
    gstd_volume_handle_close(&d->volume);
    d->volume = (gstd_volume_handle){.data_fd = writer.descriptors[0],
                                     .writer_fd = writer.descriptors[1],
                                     .identity = writer.volume};
    writer.descriptor_count = 0;
    CHECK(!gstd_network_worker_signal(&network, 0));
    while (!(ready = gstd_native_watch_poll(network.pid_fd)))
        pause_poll();
    CHECK(ready == 1);
    while (!(ready = gstd_native_watch_poll(keeper)))
        pause_poll();
    CHECK(ready == 1);
    gstd_native_exit collected;
    (void)gstd_native_exit_poll(network.pid, network.start, &collected);
    (void)gstd_native_exit_poll(observed.network.keeper_pid, observed.network.keeper_start,
                                &collected);
    close(keeper);
    gstd_network_worker_observation_close(&observed);
    gstd_network_worker_close(&network);
    gstd_filesystem_reply_close(&scope);
    gstd_filesystem_reply_close(&resolver);
    return 0;
}
/* Explicit disposable fixture only. Preparation/read-only modes deliberately
 * finalize preflight failure; --migrate uses the private schema/stop actors. */
int main(int argc, char **argv) {
    CHECK((argc == 3 ||
           ((argc == 4 || argc == 5) &&
            (!strcmp(argv[3], "--service-check") || !strcmp(argv[3], "--finish-unprepared") ||
             !strcmp(argv[3], "--resume-service-check") || !strcmp(argv[3], "--migrate") ||
             !strcmp(argv[3], "--runtime") || !strcmp(argv[3], "--recover") ||
             !strcmp(argv[3], "--leave-mutator")))) &&
          geteuid());
    const char *deployment = argc == 5 ? argv[4] : "one";
    CHECK(gstd_identifier(deployment, strnlen(deployment, 65)));
    int recover = argc >= 4 && !strcmp(argv[3], "--recover");
    int leave_mutator = argc >= 4 && !strcmp(argv[3], "--leave-mutator");
    int runtime = leave_mutator || (argc >= 4 && !strcmp(argv[3], "--runtime"));
    int migrate = argc >= 4 && !strcmp(argv[3], "--migrate");
    int resume = argc >= 4 && !strcmp(argv[3], "--resume-service-check");
    int service = migrate || resume || (argc >= 4 && !strcmp(argv[3], "--service-check"));
    int abandon = argc >= 4 && !strcmp(argv[3], "--finish-unprepared");
    alarm(300);
    struct rlimit core = {0, 0};
    CHECK(!setrlimit(RLIMIT_CORE, &core) && !prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0));
    gstd_json_initialize();
    int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    char path[4096];
    CHECK(root >= 3 && snprintf(path, sizeof(path), "%s/state.db", argv[1]) > 0);
    sqlite3 *db = NULL;
    CHECK(!gstd_state_open(path, &db));
    gstd_cache_owner cache;
    CHECK(!gstd_asset_record_cache_state(db, &cache));
    gstd_filesystem_input input = {.action = GSTD_FS_REOPEN_ROOTS,
                                   .store_id = cache.store_id,
                                   .expected_cache = cache,
                                   .descriptors = {root, -1, -1}};
    gstd_filesystem_job job = {0};
    gstd_filesystem_reply roots = {0}, configuration = {0};
    CHECK(!gstd_filesystem_launch(&input, 5000, &job) && wait_fs(&job, &roots) == 1 &&
          !roots.status);
    gstd_runtime_settings settings = GSTD_RUNTIME_SETTINGS_INITIAL;
    CHECK(!gstd_runtime_settings_load(root, &settings));
    gstd_deployment_environment environment = {.database = db,
                                               .state_fd = root,
                                               .settings = &settings,
                                               .cache_fd = roots.descriptors[0],
                                               .resource_fd = roots.descriptors[4],
                                               .native_fd = roots.descriptors[5],
                                               .broker_fd = -1,
                                               .volume_root = roots.volume_root};
    environment.volume_root.root_fd = roots.descriptors[2];
    environment.volume_root.lock_fd = roots.descriptors[3];
    if (recover) {
        CHECK(!gstd_recovery_invalidate(db));
        sqlite3_stmt *s = NULL;
        char migration[65] = {0};
        CHECK(
            sqlite3_prepare_v2(db,
                               "SELECT o.operation_id FROM operations o JOIN resource_ownership r "
                               "ON r.resource_id='migration-'||o.operation_id WHERE "
                               "o.method='migration.start' AND o.deployment_id=? AND "
                               "o.state='failed' AND o.recovery='uncertain' AND r.ownership_state "
                               "IN ('held','uncertain') "
                               "ORDER BY o.rowid DESC LIMIT 1",
                               -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_text(s, 1, deployment, -1, SQLITE_TRANSIENT);
        CHECK(sqlite3_step(s) == SQLITE_ROW);
        const char *selected = (const char *)sqlite3_column_text(s, 0);
        CHECK(selected && strlen(selected) < sizeof(migration));
        memcpy(migration, selected, strlen(selected) + 1);
        sqlite3_finalize(s);
        CHECK(sqlite3_prepare_v2(db,
                                 "SELECT json_object('state',state,'result',result,'error',error) "
                                 "FROM operations WHERE operation_id=?",
                                 -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_text(s, 1, migration, -1, SQLITE_TRANSIENT);
        CHECK(sqlite3_step(s) == SQLITE_ROW);
        char *original = strdup((const char *)sqlite3_column_text(s, 0));
        CHECK(original);
        sqlite3_finalize(s);
        json_t *message =
            json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s}}", "version", "major", 1, "minor", 0,
                      "requestId", "recover", "method", "deployment.stop", "idempotencyKey",
                      argv[2], "params", "deploymentId", deployment);
        gstd_request request;
        char operation[65];
        CHECK(message && !gstd_request_parse(message, &request));
        gstd_runtime_manager *manager = gstd_test_runtime_reopened(&environment);
        CHECK(manager);
        int error = 1;
        json_t *receipt = gstd_runtime_manager_submit(manager, &request, &error);
        const char *accepted_id = json_string_value(json_object_get(receipt, "operationId"));
        CHECK(!error && accepted_id && strlen(accepted_id) < sizeof(operation));
        memcpy(operation, accepted_id, strlen(accepted_id) + 1);
        json_decref(receipt);
        int ready = 0, helper_observed = 0;
        do {
            gstd_runtime_manager_poll(manager);
            if (!helper_observed && gstd_test_runtime_schema_observed(manager)) {
                fputs("recovery observed schema helper\n", stderr);
                CHECK(!fflush(stderr));
                helper_observed = 1;
            }
            CHECK(sqlite3_prepare_v2(db,
                                     "SELECT o.state,j.state FROM operations o JOIN jobs j "
                                     "ON j.operation_id=o.operation_id WHERE o.operation_id=?",
                                     -1, &s, NULL) == SQLITE_OK);
            sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
            CHECK(sqlite3_step(s) == SQLITE_ROW);
            const char *state = (const char *)sqlite3_column_text(s, 0),
                       *job_state = (const char *)sqlite3_column_text(s, 1);
            CHECK(strcmp(state, "failed") && strcmp(state, "cancelled"));
            ready = !strcmp(state, "succeeded") && !strcmp(job_state, "done");
            sqlite3_finalize(s);
            if (!ready)
                pause_poll();
        } while (!ready);
        gstd_runtime_manager_close(manager);
        CHECK(sqlite3_prepare_v2(db,
                                 "SELECT json_object('state',state,'result',result,'error',error) "
                                 "FROM operations WHERE operation_id=?",
                                 -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_text(s, 1, migration, -1, SQLITE_TRANSIENT);
        CHECK(sqlite3_step(s) == SQLITE_ROW);
        CHECK(!strcmp(original, (const char *)sqlite3_column_text(s, 0)));
        sqlite3_finalize(s);
        free(original);
        gstd_runtime_settings_clear(&settings);
        gstd_filesystem_reply_close(&roots);
        json_decref(message);
        CHECK(sqlite3_close(db) == SQLITE_OK);
        close(root);
        puts("{\"migrationRecovery\":true,\"originalOutcomePreserved\":true,"
             "\"createOrStartDispatched\":false,\"volumes\":\"retained\",\"publicCapability\":"
             "false}");
        return 0;
    }
    json_t *message =
        json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:b}}", "version", "major", 1, "minor", 0,
                  "requestId", "private-migration", "method", "migration.start", "idempotencyKey",
                  argv[2], "params", "deploymentId", deployment, "targetVersion",
                  "gitstore-06152d-schema-012", "backupAcknowledged", 1);
    gstd_request request;
    char operation[65], scheduled[65], config_id[65], control_id[65];
    CHECK(message && !gstd_request_parse(message, &request));
    gstd_accept_status accepted = gstd_accept(db, &request, NULL, NULL, operation);
    CHECK(accepted == ((abandon || resume) ? GSTD_ACCEPT_REPLAY : GSTD_ACCEPT_NEW));
    if (abandon) {
        CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
        gstd_migration_execution pending = {0};
        CHECK(!gstd_migration_execution_read(db, operation, &pending));
        sqlite3_stmt *s = NULL;
        CHECK(sqlite3_prepare_v2(
                  db,
                  "SELECT count(*),sum(json_extract(backend_identity,'$.kind')='configuration') "
                  "FROM resource_ownership "
                  "WHERE operation_id=?",
                  -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
        CHECK(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 2 &&
              sqlite3_column_int(s, 1) == 1);
        sqlite3_finalize(s);
        CHECK(
            sqlite3_prepare_v2(
                db,
                "UPDATE resource_ownership SET ownership_state='released',"
                "lifecycle_stage='retained' WHERE operation_id=? AND resource_type='configuration' "
                "AND resource_id='cfg-'||operation_id AND ownership_state='held' "
                "AND json_type(backend_identity,'$.native') IS NULL "
                "AND json_type(backend_identity,'$.graph') IS NULL",
                -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
        CHECK(sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1);
        sqlite3_finalize(s);
        CHECK(!gstd_migration_finish(db, operation, 0, 1));
        CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
        CHECK(!gstd_schedule_finish(db, operation));
        gstd_migration_execution_clear(&pending);
        gstd_runtime_settings_clear(&settings);
        gstd_filesystem_reply_close(&roots);
        json_decref(message);
        CHECK(sqlite3_close(db) == SQLITE_OK);
        close(root);
        puts("{\"unpreparedMigrationFinalized\":true,\"nativeDispatched\":false}");
        return 0;
    }
    if (!resume)
        CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(operation, scheduled));
    if (runtime) {
        gstd_migration_runtime actor = {0};
        CHECK(!gstd_migration_runtime_begin(&environment, operation, &actor));
        int ready;
        do {
            ready = gstd_migration_runtime_poll(&actor, -1);
            json_t *control =
                leave_mutator && !ready ? gstd_migration_inspect(db, operation) : NULL;
            const char *phase = json_string_value(json_object_get(control, "stage"));
            if (phase && !strcmp(phase, "prepared") && !actor.datastore.helper.stage) {
                CHECK(!gstd_datastore_helper_begin(&actor.datastore, GSTD_EXEC_SCHEMA_MIGRATE,
                                                   &actor.observed.network, actor.boot,
                                                   actor.resolver_fd, -1, -1));
                gstd_helper *helper = &actor.datastore.helper.helper;
                int state;
                do {
                    state = gstd_helper_poll(helper, -1);
                    if (!state)
                        pause_poll();
                } while (!state);
                CHECK(state == 1 && gstd_helper_identity_ready(helper));
                CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
                CHECK(!gstd_deployment_helper_created(db, &actor.execution.target,
                                                      helper->context.backend_id, helper));
                CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
                CHECK(!gstd_helper_permit_start(helper));
                while (helper->worker.worker) {
                    state = gstd_helper_poll(helper, -1);
                    CHECK(state >= 0);
                    if (!state) {
                        struct timespec delay = {0, 1000000};
                        nanosleep(&delay, NULL);
                    }
                }
                if (helper->observed.running && gstd_native_watch_poll(helper->death_fd) == 0 &&
                    !syscall(SYS_pidfd_send_signal, helper->death_fd, SIGSTOP, NULL, 0)) {
                    int paused = 0;
                    for (unsigned i = 0; i < 20 && !paused; i++) {
                        paused = helper_paused(helper);
                        if (!paused)
                            pause_poll();
                    }
                    if (paused) {
                        puts("{\"migrationHelperPaused\":true,\"publicCapability\":false}");
                        CHECK(!fflush(stdout));
                        for (;;)
                            pause();
                    }
                    (void)syscall(SYS_pidfd_send_signal, helper->death_fd, SIGCONT, NULL, 0);
                }
            }
            json_decref(control);
            if (!ready)
                pause_poll();
        } while (!ready);
        CHECK(ready == 1 && !actor.outcome && actor.cleanup_observed && actor.schema_succeeded);
        CHECK(!gstd_schedule_finish(db, operation));
        CHECK(!gstd_migration_runtime_close(&actor));
        gstd_runtime_settings_clear(&settings);
        gstd_filesystem_reply_close(&roots);
        json_decref(message);
        CHECK(sqlite3_close(db) == SQLITE_OK);
        close(root);
        puts("{\"privateStandaloneMigration\":true,\"sourceStopped\":true,\"historyRows\":12,"
             "\"migrationCompleted\":true,\"publicCapability\":false}");
        return 0;
    }
    CHECK(!resource_name(config_id, "cfg-", operation) &&
          !resource_name(control_id, "migration-", operation));
    gstd_migration_execution execution = {0};
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!gstd_migration_execution_read(db, operation, &execution));
    if (!resume)
        CHECK(!gstd_deployment_resource_reserve(db, &execution.target, config_id, "configuration"));
    CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    input = (gstd_filesystem_input){.action = GSTD_FS_SCOPE,
                                    .manifest = execution.target.manifest,
                                    .store_id = execution.target.store_id,
                                    .operation_id = operation,
                                    .deployment_id = execution.target.deployment_id,
                                    .generation = execution.target.generation,
                                    .resource_id = config_id,
                                    .resource_kind = "configuration",
                                    .descriptors = {roots.descriptors[4], -1, -1}};
    if (!resume) {
        CHECK(!gstd_filesystem_launch(&input, 10000, &job) && wait_fs(&job, &configuration) == 1 &&
              !configuration.status);
        CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
        CHECK(!gstd_deployment_scope_record(db, &execution.target, config_id, &configuration));
        CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    }
    gstd_datastore datastore = {0};
    if (resume) {
        CHECK(!reopen_scope(&environment, &execution.target, config_id, "configuration",
                            &configuration));
        CHECK(
            !gstd_datastore_recovery_seed(&environment, &execution.target, config_id, &datastore));
        char service_id[65];
        CHECK(!resource_name(service_id, "run-scylladb-", operation));
        json_t *service_identity = owned_identity(db, operation, service_id);
        int live = service_identity && json_object_get(service_identity, "native");
        json_decref(service_identity);
        if (live) {
            CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
            CHECK(gstd_scylla_migration_volume_reserve(db, operation, datastore.volume_id) ==
                  GSTD_ACCEPT_REPLAY);
            gstd_volume_identity expected;
            CHECK(gstd_scylla_volume_record_begin(db, &environment.volume_root, datastore.volume_id,
                                                  operation, datastore.producer_id,
                                                  &expected) == 1);
            CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
            gstd_filesystem_input observe = {.action = GSTD_FS_OBSERVE_DATASTORE,
                                             .manifest = execution.target.manifest,
                                             .volume_root = environment.volume_root,
                                             .volume_id = datastore.volume_id,
                                             .producer_id = datastore.producer_id,
                                             .volume_expected = expected,
                                             .descriptors = {environment.volume_root.root_fd,
                                                             environment.volume_root.lock_fd, -1}};
            gstd_filesystem_reply observed = {0};
            CHECK(!gstd_filesystem_launch(&observe, 5000, &job) && wait_fs(&job, &observed) == 1 &&
                  !observed.status && observed.descriptor_count == 1);
            datastore.volume = (gstd_volume_handle){
                .data_fd = observed.descriptors[0], .writer_fd = -1, .identity = observed.volume};
        } else {
            gstd_migration_datastore allocation = {0};
            CHECK(!gstd_migration_datastore_begin(db, &environment.volume_root, operation,
                                                  &allocation));
            int ready;
            do {
                ready = gstd_migration_datastore_poll(&allocation, -1);
                if (!ready)
                    pause_poll();
            } while (!ready);
            CHECK(ready == 1 && !allocation.outcome);
            datastore.volume = allocation.volume;
            allocation.volume.data_fd = allocation.volume.writer_fd = -1;
            memcpy(datastore.volume_id, allocation.volume_id, sizeof(datastore.volume_id));
            memcpy(datastore.producer_id, allocation.producer_id, sizeof(datastore.producer_id));
            CHECK(!gstd_migration_datastore_close(&allocation));
        }
        for (unsigned i = 0; i < 3; i++) {
            gstd_deployment_workload *w = &datastore.workloads[i];
            json_t *identity = owned_identity(db, operation, w->resource);
            if (i && (json_object_get(identity, "helperControl") ||
                      json_object_get(identity, "native"))) {
                char boot[37];
                CHECK(!gstd_native_boot_id(boot));
                CHECK(!gstd_deployment_helper_receipt_read(&execution.target, w->resource, identity,
                                                           boot, environment.native_fd));
            }
            json_decref(identity);
            CHECK(
                !reopen_scope(&environment, &execution.target, w->resource, "workload", &w->scope));
            CHECK(w->scope.descriptor_count == 4);
            w->rootfs_fd = w->scope.descriptors[3];
            w->scope.descriptor_count = 3;
        }
    } else {
        CHECK(
            !gstd_datastore_prepare_begin(&environment, &execution.target, config_id, &datastore));
        for (;;) {
            int ready = gstd_datastore_prepare_poll(&datastore, -1);
            CHECK(ready >= 0);
            if (ready)
                break;
            struct timespec delay = {0, 10000000};
            nanosleep(&delay, NULL);
        }
    }
    fprintf(stderr, "standalone preparation outcome=%d detail=%d\n", datastore.outcome,
            datastore.detail);
    CHECK(!datastore.outcome && datastore.index == 3 &&
          (datastore.volume.writer_fd >= 3 || resume));
    for (unsigned i = 0; !resume && i < 3; i++) {
        CHECK(datastore.workloads[i].rootfs_fd >= 3 && !datastore.workloads[i].launched &&
              !datastore.workloads[i].native.configuration);
        errno = 0;
        CHECK(faccessat(environment.native_fd, datastore.workloads[i].resource, F_OK,
                        AT_SYMLINK_NOFOLLOW) < 0 &&
              errno == ENOENT);
    }
    if (service)
        CHECK(!service_check(&datastore, &configuration, resume, migrate));
    /* All dispatched native actors are observed deleted, workers are reaped, and this parent owns
     * the writer. Release only these preparation resources and retain their
     * scopes/data. Record failure rather than inventing completed migration. */
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(db,
                             "UPDATE resource_ownership SET ownership_state='released',"
                             "lifecycle_stage=CASE WHEN resource_type='asset-root' THEN 'released' "
                             "ELSE 'retained' END "
                             "WHERE operation_id=? AND resource_id!=? "
                             "AND ownership_state='held'",
                             -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, control_id, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == (service ? 9 : 8));
    sqlite3_finalize(s);
    for (unsigned i = 0; i < 3; i++)
        CHECK(!gstd_asset_record_unpin(db, datastore.workloads[i].asset_resource));
    CHECK(!gstd_migration_finish(db, operation, migrate, 1));
    CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!gstd_schedule_finish(db, operation));
    CHECK(!gstd_datastore_observers_close(&datastore));
    gstd_migration_execution_clear(&execution);
    gstd_filesystem_reply_close(&configuration);
    gstd_runtime_settings_clear(&settings);
    gstd_filesystem_reply_close(&roots);
    json_decref(message);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    close(root);
    if (migrate)
        puts("{\"privateStandaloneMigration\":true,\"sourceStopped\":true,\"historyRows\":12,"
             "\"migrationCompleted\":true,\"publicCapability\":false}");
    else if (service)
        puts("{\"migrationReadOnlyPreflight\":true,\"sourceStopped\":true,\"historyRows\":12,"
             "\"migrationCompleted\":false}");
    else
        puts("{\"migrationPreparation\":true,\"materializations\":3,\"nativeDispatched\":false,"
             "\"migrationCompleted\":false,\"publicCapability\":false}");
    return 0;
}
