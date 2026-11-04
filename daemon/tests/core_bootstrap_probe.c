#define _GNU_SOURCE 1
#include "bootstrap.h"
#include "bootstrap_http.h"
#include "bootstrap_job.h"
#include "execution_plan.h"
#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "native_exit.h"
#include "native_job.h"
#include "native_population.h"
#include "network_records.h"
#include "network_worker.h"
#include "volume_records.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
/* Explicit disposable Linux integration fixture. No public dispatcher or caller
 * container configuration. Supplied rootfs copies must already have passed the
 * image library's manifest-bound materialization. Failures retain all resources
 * and their ledger; never infer rollback or delete a possibly running workload. */
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "core fixture line %d\n", __LINE__);                                   \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static sqlite3 *db;
static json_t *manifest;
static char operation[65], store[33], boot[37];
static pid_t owner;
static gstd_bootstrap_graph graph;
static gstd_bootstrap_request preparation;
static int broker, scope;
static const char *credential_path;
static gstd_network_worker network_worker = GSTD_NETWORK_WORKER_INITIAL;
static gstd_network_worker_observation network = GSTD_NETWORK_OBSERVATION_INITIAL;
typedef struct {
    const char *name;
    char id[65];
    gstd_execution_inputs inputs;
    gstd_native_context context;
    gstd_native_observation observed;
    gstd_native_population population;
} workload;
static workload workloads[3]; /* Git, API, controller: startup order */
static void pause_ms(unsigned ms) {
    struct timespec delay = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    nanosleep(&delay, NULL);
}
static int sql(const char *text) {
    return sqlite3_exec(db, text, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
static int reserve(const char *id, const char *type) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO resource_ownership "
            "VALUES(?,?,'fixture',?,'reserved',NULL,'held',strftime('%Y-%m-%dT%H:%M:%SZ','now'))",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, operation, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok ? 0 : -1;
}
static int ledger(const workload *w, const char *stage, const char *uid) {
    json_t *record =
        json_pack("{s:s,s:s,s:s,s:I,s:I,s:s,s:I,s:i,s:i}", "kind", "libcrun", "id", w->id, "bootId",
                  boot, "pid", (json_int_t)w->observed.pid, "startTime",
                  (json_int_t)w->observed.start_time, "enrollmentUid",
                  uid ? uid : graph.controller_uid, "generation", (json_int_t)graph.generation,
                  "bootstrapStage", (int)graph.stage, "bootstrapOutcome", graph.outcome);
    char *bytes = record ? gstd_json_dump(record) : NULL;
    sqlite3_stmt *s = NULL;
    int ok = bytes && sqlite3_prepare_v2(
                          db,
                          "UPDATE resource_ownership SET lifecycle_stage=?,backend_identity=? "
                          "WHERE resource_id=? AND operation_id=? AND ownership_state='held'",
                          -1, &s, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(s, 1, stage, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, bytes, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, w->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 4, operation, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    }
    sqlite3_finalize(s);
    free(bytes);
    json_decref(record);
    return ok ? 0 : -1;
}
static int native(workload *w, gstd_native_action action) {
    gstd_native_job job = {0};
    gstd_native_status result;
    if (gstd_native_launch(&w->context, action, 10000, &job))
        return -1;
    int ready;
    while (!(ready = gstd_native_poll(&job, -1, &result, &w->observed)))
        pause_ms(1);
    if (ready != 1 || result != GSTD_NATIVE_OK) {
        fprintf(stderr, "core %s action %d status %d nativeError %d\n", w->name, action,
                ready == 1 ? (int)result : -1, w->observed.native_error);
        return -1;
    }
    return 0;
}
static int advance(int result, const char *uid) {
    if (gstd_bootstrap_graph_observe(&graph, 1, graph.stage, result, uid))
        return -1;
    const workload *api = &workloads[1];
    return ledger(api,
                  api->observed.running       ? "running"
                  : api->context.expected_pid ? "allocated"
                                              : "reserved",
                  NULL);
}
typedef gstd_bootstrap_job_reply private_reply;
static int private_worker(int action, private_reply *reply) {
    json_t *source = json_pack("{s:s,s:s}", "kind", "protected-file", "path", credential_path);
    if (!source)
        return -1;
    gstd_bootstrap_job_input input = {.preparation = preparation,
                                      .kind = action == -1   ? GSTD_BOOT_JOB_PREPARE
                                              : action == -2 ? GSTD_BOOT_JOB_BIND
                                                             : GSTD_BOOT_JOB_HTTP,
                                      .broker_fd = broker,
                                      .allow_initial = action == -1,
                                      .source = source,
                                      .action = action >= 0 ? (gstd_bootstrap_http_action)action
                                                            : GSTD_BOOT_API_READY,
                                      .network = action >= 0 ? &network.network : NULL,
                                      .uid = reply->uid,
                                      .api_boot = boot,
                                      .api_pid = workloads[1].context.expected_pid,
                                      .api_start = workloads[1].context.expected_start_time};
    gstd_bootstrap_job job = {0};
    int launched = gstd_bootstrap_job_launch(&input, 25000, &job);
    json_decref(source);
    if (launched)
        return -1;
    int ready;
    while (!(ready = gstd_bootstrap_job_poll(&job, -1, reply)))
        pause_ms(1);
    return ready == 1 ? 0 : -1;
}
static int wait_ready(gstd_bootstrap_http_action action) {
    struct timespec began, now;
    if (clock_gettime(CLOCK_MONOTONIC, &began))
        return -1;
    for (unsigned attempt = 0; attempt < 300; attempt++) {
        if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec - began.tv_sec >= 60)
            return -1;
        workload *current = &workloads[action == GSTD_BOOT_CONTROLLER_READY ? 2 : 1];
        if (native(current, GSTD_NATIVE_OBSERVE) || !current->observed.running)
            return -1;
        private_reply reply = {0};
        if (private_worker((int)action, &reply))
            return -1;
        if (reply.status == GSTD_BOOT_HTTP_READY) {
            printf("{\"readyAction\":%d}\n", action);
            fflush(stdout);
            return 0;
        }
        if (reply.status != GSTD_BOOT_HTTP_PENDING) {
            fprintf(stderr, "core HTTP action %d status %d\n", action, reply.status);
            return -1;
        }
        pause_ms(200);
    }
    return -1;
}
static int child_dir(int root, const char *name) {
    if (mkdirat(root, name, 0700))
        return -1;
    return openat(root, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}
static int role(const char *name) { return openat(scope, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW); }
static int launch(workload *w) {
    w->inputs.manifest = manifest;
    w->inputs.component = w->name;
    w->inputs.network = &network.network;
    w->context.registered_inputs = &w->inputs;
    w->context.configuration = gstd_execution_build(&w->inputs);
    if (!w->context.configuration || native(w, GSTD_NATIVE_CREATE))
        return -1;
    w->context.expected_pid = w->observed.pid;
    w->context.expected_start_time = w->observed.start_time;
    if (gstd_native_population_open(w->id, w->observed.pid, w->observed.start_time, boot,
                                    &w->population) ||
        gstd_native_population_empty(&w->population) != 0)
        return -1;
    if (ledger(w, "allocated", NULL) || native(w, GSTD_NATIVE_START) ||
        native(w, GSTD_NATIVE_OBSERVE) || !w->observed.running || ledger(w, "running", NULL))
        return -1;
    if (gstd_native_population_matches(&w->population, w->id, w->context.expected_pid,
                                       w->context.expected_start_time, boot))
        return -1;
    if (!strcmp(w->name, "git")) {
        struct stat held, actual;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%ld/fd/3", (long)w->context.expected_pid);
        if (fstat(w->context.writer_lease_fd, &held) || stat(path, &actual) ||
            held.st_dev != actual.st_dev || held.st_ino != actual.st_ino)
            return -1;
    }
    printf("{\"launched\":\"%s\",\"pid\":%ld}\n", w->name, (long)w->observed.pid);
    fflush(stdout);
    return 0;
}
static int cleanup(int root, char **argv) {
    sqlite3_stmt *s = NULL;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT store_id FROM query_meta", -1, &s, NULL) == SQLITE_OK &&
            sqlite3_step(s) == SQLITE_ROW);
    snprintf(store, sizeof(store), "%s", sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    REQUIRE(sqlite3_prepare_v2(db,
                               "SELECT operation_id FROM operations WHERE deployment_id='fixture' "
                               "AND method='deployment.start' AND caller_uid=?",
                               -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, (sqlite3_int64)geteuid());
    REQUIRE(sqlite3_step(s) == SQLITE_ROW);
    snprintf(operation, sizeof(operation), "%s", sqlite3_column_text(s, 0));
    REQUIRE(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    int net_scope = openat(root, "network", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    gstd_network_worker_request input = {.manifest = manifest,
                                         .store_id = store,
                                         .resource_id = "core-network",
                                         .operation_id = operation,
                                         .scope_fd = net_scope};
    REQUIRE(sqlite3_prepare_v2(
                db,
                "SELECT backend_identity FROM resource_ownership WHERE resource_id='core-network' "
                "AND operation_id=? AND ownership_state='held'",
                -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
    REQUIRE(sqlite3_step(s) == SQLITE_ROW);
    json_t *identity =
        gstd_json_parse(sqlite3_column_text(s, 0), (size_t)sqlite3_column_bytes(s, 0));
    sqlite3_finalize(s);
    REQUIRE(identity && !gstd_network_worker_reopen(&input, identity, &network_worker, &network));
    json_decref(identity);
    char api_id[65];
    snprintf(api_id, sizeof(api_id), "core-api-%.16s", store);
    REQUIRE(sqlite3_prepare_v2(db,
                               "SELECT backend_identity FROM resource_ownership WHERE "
                               "resource_id=? AND operation_id=? AND ownership_state='held'",
                               -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, api_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, operation, -1, SQLITE_TRANSIENT);
    REQUIRE(sqlite3_step(s) == SQLITE_ROW);
    json_t *saved = gstd_json_parse(sqlite3_column_text(s, 0), (size_t)sqlite3_column_bytes(s, 0));
    sqlite3_finalize(s);
    REQUIRE(saved);
    /* Retain assigned/partial UID and graph outcome across this test-only
     * recovered stop; stopping resources is never permission to erase them. */
    graph.generation = (uint64_t)json_integer_value(json_object_get(saved, "generation"));
    graph.stage =
        (gstd_bootstrap_stage)json_integer_value(json_object_get(saved, "bootstrapStage"));
    graph.outcome = (int)json_integer_value(json_object_get(saved, "bootstrapOutcome"));
    const char *known_uid = json_string_value(json_object_get(saved, "enrollmentUid"));
    REQUIRE(!known_uid || strlen(known_uid) < sizeof(graph.controller_uid));
    if (known_uid)
        memcpy(graph.controller_uid, known_uid, strlen(known_uid) + 1);
    json_decref(saved);
    const char *names[] = {"git", "api", "controller"};
    const int roots[] = {3, 2, 4};
    int state = openat(root, "native-state", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    for (int i = 2; i >= 0; i--) {
        workload *w = &workloads[i];
        w->name = names[i];
        w->population = (gstd_native_population)GSTD_NATIVE_POPULATION_INITIAL;
        snprintf(w->id, sizeof(w->id), "core-%s-%.16s", names[i], store);
        REQUIRE(sqlite3_prepare_v2(db,
                                   "SELECT backend_identity FROM resource_ownership WHERE "
                                   "resource_id=? AND operation_id=? AND ownership_state='held'",
                                   -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_text(s, 1, w->id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, operation, -1, SQLITE_TRANSIENT);
        REQUIRE(sqlite3_step(s) == SQLITE_ROW);
        json_t *record =
            gstd_json_parse(sqlite3_column_text(s, 0), (size_t)sqlite3_column_bytes(s, 0));
        sqlite3_finalize(s);
        const char *saved_boot = json_string_value(json_object_get(record, "bootId"));
        REQUIRE(record && saved_boot && !strcmp(saved_boot, boot));
        w->context = (gstd_native_context){
            .backend_id = w->id,
            .component = w->name,
            .boot_id = boot,
            .control_owner = owner,
            .state_fd = state,
            .bundle_fd = openat(root, w->name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW),
            .rootfs_fd = open(argv[roots[i]], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW),
            .writer_lease_fd = -1,
            .expected_pid = (pid_t)json_integer_value(json_object_get(record, "pid")),
            .expected_start_time =
                (uint64_t)json_integer_value(json_object_get(record, "startTime"))};
        json_decref(record);
        REQUIRE(!native(w, GSTD_NATIVE_OBSERVE) && !ledger(w, "stopping", NULL));
        if (!w->observed.stopped)
            REQUIRE(!native(w, GSTD_NATIVE_TERM));
        unsigned n;
        for (n = 0; n < 1000; n++) {
            REQUIRE(!native(w, GSTD_NATIVE_OBSERVE));
            if (w->observed.stopped)
                break;
            pause_ms(10);
        }
        /* Recovered init is reaped by PID 1. Non-forced native delete must also
         * succeed; merely seeing stopped never releases the writer reservation. */
        REQUIRE(n < 1000 && !native(w, GSTD_NATIVE_DELETE) && !ledger(w, "retained", NULL));
    }
    REQUIRE(!gstd_network_worker_signal(&network_worker, 0));
    puts("{\"recoveredFixtureStopped\":true,\"ownership\":\"retained\"}");
    return 0;
}
int main(int argc, char **argv) {
    int recovering = argc == 7 && !strcmp(argv[6], "cleanup");
    REQUIRE((argc == 6 || recovering) && geteuid() != 0);
    umask(077);
    owner = getpid();
    REQUIRE(!prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0));
    struct rlimit core = {0, 0};
    REQUIRE(!setrlimit(RLIMIT_CORE, &core));
    gstd_json_initialize();
    REQUIRE(!gstd_native_boot_id(boot));
    int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    REQUIRE(root >= 3 && !fstat(root, &info) && info.st_uid == geteuid() &&
            (info.st_mode & 07777) == 0700);
    char path[4096], credentials[4096];
    REQUIRE(snprintf(path, sizeof(path), "%s/state.db", argv[1]) > 0 &&
            !gstd_state_open(path, &db));
    json_t *components = json_pack("[s,s,s]", "api", "git", "controller");
#if defined(__aarch64__)
    manifest = gstd_asset_manifest(components, "arm64");
    const char *platform = "linux-arm64";
#else
    manifest = gstd_asset_manifest(components, "amd64");
    const char *platform = "linux-amd64";
#endif
    REQUIRE(manifest);
    if (recovering)
        return cleanup(root, argv);
    sqlite3_stmt *s = NULL;
    REQUIRE(sqlite3_prepare_v2(db, "INSERT INTO profiles VALUES('core','1',?,?,?)", -1, &s, NULL) ==
            SQLITE_OK);
    sqlite3_bind_text(s, 1, json_string_value(json_object_get(manifest, "serverRevision")), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, json_string_value(json_object_get(manifest, "profileDigest")), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, platform, -1, SQLITE_TRANSIENT);
    REQUIRE(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    json_t *message = json_pack(
        "{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:s}}", "version", "major", 1, "minor", 0, "requestId",
        "core-fixture", "method", "deployment.start", "idempotencyKey", "core-fixture", "params",
        "deploymentId", "fixture", "profileId", "core", "profileVersion", "1");
    gstd_request request;
    REQUIRE(message && !gstd_request_parse(message, &request) &&
            gstd_accept(db, &request, components, "repositories", operation) == GSTD_ACCEPT_NEW);
    REQUIRE(sqlite3_prepare_v2(db, "SELECT store_id FROM query_meta", -1, &s, NULL) == SQLITE_OK &&
            sqlite3_step(s) == SQLITE_ROW);
    snprintf(store, sizeof(store), "%s", sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    REQUIRE(!sql("BEGIN IMMEDIATE") && !reserve("core-network", "network"));
    const char *names[] = {"git", "api", "controller"};
    for (unsigned i = 0; i < 3; i++) {
        workload *w = &workloads[i];
        w->name = names[i];
        w->population = (gstd_native_population)GSTD_NATIVE_POPULATION_INITIAL;
        snprintf(w->id, sizeof(w->id), "core-%s-%.16s", names[i], store);
        REQUIRE(!reserve(w->id, "workload"));
    }
    REQUIRE(!sql("COMMIT"));
    REQUIRE(!gstd_bootstrap_graph_begin(manifest, "memdb", 1, &graph) &&
            !ledger(&workloads[1], "reserved", NULL));
    int state = child_dir(root, "native-state"), volumes = child_dir(root, "volumes");
    scope = child_dir(root, "configuration");
    int credential_dir = child_dir(root, "credentials");
    REQUIRE(state >= 3 && volumes >= 3 && scope >= 3 && credential_dir >= 3);
    close(credential_dir);
    REQUIRE(snprintf(credentials, sizeof(credentials), "%s/credentials/bootstrap.json", argv[1]) >
            0);
    credential_path = credentials;
    broker = open(argv[5], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(broker >= 3);
    preparation = (gstd_bootstrap_request){.manifest = manifest,
                                           .store_id = store,
                                           .deployment_id = "fixture",
                                           .operation_id = operation,
                                           .datastore = "memdb",
                                           .generation = 1,
                                           .control_owner = owner,
                                           .scope_fd = scope};
    private_reply reply = {0};
    REQUIRE(!private_worker(-1, &reply) && reply.status == 0);
    REQUIRE(!advance(0, NULL));
    gstd_volume_root volume_owner = {.lock_fd = -1};
    gstd_volume_handle volume = {.data_fd = -1, .writer_fd = -1};
    gstd_volume_identity expected;
    char producer[65];
    REQUIRE(gstd_volume_root_lock(volumes, store, &volume_owner) == GSTD_VOLUME_OK &&
            !sql("BEGIN IMMEDIATE") &&
            gstd_volume_record_begin(db, &volume_owner, "repositories", operation, producer,
                                     &expected) == 0 &&
            !sql("COMMIT") &&
            gstd_volume_acquire(volumes, &volume_owner, "repositories", producer, 1, NULL,
                                &volume) == GSTD_VOLUME_OK &&
            !sql("BEGIN IMMEDIATE") &&
            !gstd_volume_record_ready(db, &volume_owner, "repositories", operation, &volume) &&
            !sql("COMMIT"));
    int net_scope = child_dir(root, "network");
    REQUIRE(net_scope >= 3);
    gstd_network_worker_request net_request = {.manifest = manifest,
                                               .store_id = store,
                                               .resource_id = "core-network",
                                               .operation_id = operation,
                                               .scope_fd = net_scope};
    REQUIRE(gstd_network_worker_prepare(&net_request, &network_worker) == GSTD_NETWORK_OK &&
            !sql("BEGIN IMMEDIATE") &&
            !gstd_network_record_bind(db, &net_request, &network_worker) && !sql("COMMIT") &&
            !gstd_network_worker_permit(&network_worker));
    int ready = 0;
    for (unsigned i = 0; i < 1000 && !ready; i++) {
        ready = gstd_network_worker_ready(&network_worker, &network);
        if (!ready)
            pause_ms(10);
    }
    REQUIRE(ready == 1 && !sql("BEGIN IMMEDIATE") &&
            !gstd_network_record_running(db, &network_worker, &network));
    for (unsigned i = 0; i < 3; i++)
        REQUIRE(!gstd_network_attachment_reserve(db, &network_worker, &network, workloads[i].id));
    REQUIRE(!sql("COMMIT"));
    int resolver = openat(root, "resolver.conf", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    static const char dns[] = "nameserver 10.0.2.3\noptions timeout:2 attempts:2\n";
    REQUIRE(resolver >= 3 && write(resolver, dns, sizeof(dns) - 1) == sizeof(dns) - 1 &&
            !fsync(resolver));
    close(resolver);
    resolver = openat(root, "resolver.conf", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    REQUIRE(resolver >= 3);
    /* CLI roots ordered API, Git, controller; native launch order is Git first. */
    const int root_args[] = {3, 2, 4};
    for (unsigned i = 0; i < 3; i++) {
        workload *w = &workloads[i];
        int bundle = child_dir(root, w->name);
        int rootfs = open(argv[root_args[i]], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        REQUIRE(bundle >= 3 && rootfs >= 3);
        w->inputs.rootfs_fd = rootfs;
        for (unsigned j = 0; j < GSTD_MOUNT_COUNT; j++)
            w->inputs.role_fds[j] = -1;
        w->inputs.role_fds[GSTD_MOUNT_RESOLVER] = resolver;
        w->context = (gstd_native_context){.backend_id = w->id,
                                           .component = w->name,
                                           .boot_id = boot,
                                           .state_fd = state,
                                           .bundle_fd = bundle,
                                           .rootfs_fd = rootfs,
                                           .writer_lease_fd = -1,
                                           .control_owner = owner};
    }
    workloads[0].inputs.role_fds[GSTD_MOUNT_BASE] = role("git.toml");
    workloads[0].inputs.role_fds[GSTD_MOUNT_REPOSITORIES] = volume.data_fd;
    workloads[0].context.writer_lease_fd = volume.writer_fd;
    workloads[1].inputs.role_fds[GSTD_MOUNT_BASE] = role("api-base.toml");
    workloads[1].inputs.role_fds[GSTD_MOUNT_OVERLAY] = role("api-owned.toml");
    workloads[1].inputs.role_fds[GSTD_MOUNT_POLICY] = role("policy.yaml");
    workloads[1].inputs.role_fds[GSTD_MOUNT_USERS] = role("users.yaml");
    REQUIRE(!launch(&workloads[0]) && !advance(0, NULL));
    REQUIRE(!close(volume.writer_fd));
    volume.writer_fd = -1;
    workloads[0].context.writer_lease_fd = -1;
    gstd_volume_handle contender = {.data_fd = -1, .writer_fd = -1};
    REQUIRE(gstd_volume_acquire(volumes, &volume_owner, "repositories", producer, 0,
                                &volume.identity, &contender) == GSTD_VOLUME_HELD);
    REQUIRE(!launch(&workloads[1]) && !advance(0, NULL));
    REQUIRE(!wait_ready(GSTD_BOOT_API_READY) && !advance(0, NULL));
    REQUIRE(!wait_ready(GSTD_BOOT_GIT_READY) && !advance(0, NULL));
    REQUIRE(!private_worker(GSTD_BOOT_CONTROLLER_ENROLL, &reply));
    /* Persist even a known partial UID before interpreting an uncertain result. */
    REQUIRE(!advance(reply.status, reply.uid));
    REQUIRE(reply.status == GSTD_BOOT_HTTP_READY && *reply.uid);
    REQUIRE(!private_worker(-2, &reply) && reply.status == 0);
    REQUIRE(!advance(0, NULL));
    workloads[2].inputs.role_fds[GSTD_MOUNT_BASE] = role("controller-base.toml");
    workloads[2].inputs.role_fds[GSTD_MOUNT_OVERLAY] = role("controller-owned.toml");
    workloads[2].inputs.role_fds[GSTD_MOUNT_SIGNING_RECORD] = role("controller-signing.json");
    REQUIRE(!launch(&workloads[2]) && !advance(0, NULL));
    REQUIRE(!wait_ready(GSTD_BOOT_CONTROLLER_READY) && !advance(0, NULL) &&
            graph.stage == GSTD_BOOT_COMPLETE);
    for (int i = 2; i >= 0; i--) {
        workload *w = &workloads[i];
        REQUIRE(!ledger(w, "stopping", NULL) && !native(w, GSTD_NATIVE_TERM));
        gstd_native_exit exited;
        int done = 0;
        for (unsigned n = 0; n < 1000 && !done; n++) {
            done = gstd_native_exit_poll(w->context.expected_pid, w->context.expected_start_time,
                                         &exited);
            if (!done)
                pause_ms(10);
        }
        REQUIRE(done == 1);
        int empty = 0;
        for (unsigned n = 0; n < 1000 && !empty; n++) {
            empty = gstd_native_population_empty(&w->population);
            if (!empty)
                pause_ms(10);
        }
        if (empty != 1) {
            struct stat d, e;
            int ds = fstat(w->population.directory_fd, &d), es = fstat(w->population.events_fd, &e);
            char bytes[4096];
            errno = 0;
            ssize_t count = pread(w->population.events_fd, bytes, sizeof(bytes), 0);
            fprintf(stderr,
                    "cgroup proof status=%d dirStat=%d dirLinks=%lu eventStat=%d eventLinks=%lu "
                    "eventRead=%zd eventErrno=%d\n",
                    empty, ds, ds ? 0UL : (unsigned long)d.st_nlink, es,
                    es ? 0UL : (unsigned long)e.st_nlink, count, errno);
        }
        REQUIRE(empty == 1 && !native(w, GSTD_NATIVE_DELETE) && !ledger(w, "retained", NULL));
        gstd_native_population_close(&w->population);
    }
    gstd_volume_identity retained_identity = volume.identity;
    gstd_volume_handle_close(&volume);
    REQUIRE(gstd_volume_acquire(volumes, &volume_owner, "repositories", producer, 0,
                                &retained_identity, &volume) == GSTD_VOLUME_OK);
    REQUIRE(volume.identity.data_device == retained_identity.data_device &&
            volume.identity.data_inode == retained_identity.data_inode);
    puts("{\"terminationProof\":\"held cgroup2 inodes empty; actual Git FD3 lease; retained writer "
         "reacquired\"}");
    REQUIRE(!gstd_network_worker_signal(&network_worker, 0));
    int status;
    REQUIRE(waitpid(network_worker.pid, &status, 0) == network_worker.pid && WIFEXITED(status) &&
            !WEXITSTATUS(status));
    puts("{\"coreBootstrap\":\"passed\",\"scope\":\"private native memdb fixture; public "
         "dispatcher unverified\"}");
    sqlite3_close(db);
    return 0;
}
