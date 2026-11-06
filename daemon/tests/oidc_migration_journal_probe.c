#define _GNU_SOURCE 1
#include "oidc_migration_journal_probe.h"
#include "network_records.h"
#include "helper_owner.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "OIDC migration journal line %d: %s\n", __LINE__, #x);                \
            return -1;                                                                             \
        }                                                                                          \
    } while (0)
#define SQL(x) CHECK(sqlite3_exec(db, x, NULL, NULL, NULL) == SQLITE_OK)
static gstd_network_worker cleanup_owner = GSTD_NETWORK_WORKER_INITIAL;
static int native_may_exist;
static void cleanup_network(void) {
    if (native_may_exist)
        return; /* Preserve the owned namespace for uncertain native recovery. */
    if (cleanup_owner.pid > 0) {
        if (!gstd_network_worker_signal(&cleanup_owner, 0)) {
            struct pollfd exited = {.fd = cleanup_owner.pid_fd, .events = POLLIN};
            if (poll(&exited, 1, 10000) == 1)
                (void)waitpid(cleanup_owner.pid, NULL, 0);
        }
        gstd_network_worker_close(&cleanup_owner);
    }
}
static int scope(const gstd_deployment_environment *env, const gstd_deployment_target *target,
                 const char *id, const char *kind, gstd_filesystem_reply *reply) {
    sqlite3 *db = env->database;
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_deployment_resource_reserve(db, target, id, kind));
    SQL("COMMIT");
    gstd_filesystem_input input = {.action = GSTD_FS_SCOPE,
                                   .manifest = target->manifest,
                                   .store_id = target->store_id,
                                   .deployment_id = target->deployment_id,
                                   .operation_id = target->operation_id,
                                   .generation = target->generation,
                                   .resource_id = id,
                                   .resource_kind = kind,
                                   .descriptors = {env->resource_fd, -1, -1}};
    gstd_filesystem_job job = {0};
    CHECK(!gstd_filesystem_launch(&input, 10000, &job));
    int complete;
    while (!(complete = gstd_filesystem_poll(&job, -1, reply)))
        usleep(1000);
    CHECK(complete == 1 && !reply->status);
    if (strcmp(kind, "network")) {
        SQL("BEGIN IMMEDIATE");
        CHECK(!gstd_deployment_scope_record(db, target, id, reply));
        SQL("COMMIT");
    }
    return 0;
}
int gstd_test_oidc_migration_journal(const gstd_deployment_environment *env,
                                      const gstd_deployment_target *target,
                                      const gstd_oidc_setup *setup, int expect_dispatch,
                                      const char *native_source) {
    sqlite3 *db = env->database;
    gstd_filesystem_reply network_scope = {0}, scopes[2] = {{0}, {0}};
    char network_id[65];
    int length = snprintf(network_id, sizeof(network_id), "oidc-journal-net-%s", target->operation_id);
    CHECK(length > 0 && length < (int)sizeof(network_id));
    CHECK(!scope(env, target, network_id, "network", &network_scope));
    gstd_network_worker_request request = {.manifest = target->manifest,
                                           .store_id = target->store_id,
                                           .resource_id = network_id,
                                           .operation_id = target->operation_id,
                                           .scope_fd = network_scope.descriptors[1],
                                           .oidc_ports = &setup->ports};
    gstd_network_worker owner = GSTD_NETWORK_WORKER_INITIAL;
    gstd_network_worker_observation actual = GSTD_NETWORK_OBSERVATION_INITIAL;
    CHECK(!atexit(cleanup_network));
    CHECK(gstd_network_worker_prepare(&request, &owner) == GSTD_NETWORK_OK);
    cleanup_owner = owner;
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_network_record_bind(db, &request, &owner));
    SQL("COMMIT");
    CHECK(!gstd_network_worker_permit(&owner));
    int ready = 0;
    for (unsigned i = 0; i < 10000 && !(ready = gstd_network_worker_ready(&owner, &actual)); i++)
        usleep(1000);
    CHECK(ready == 1);
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_network_record_running(db, &owner, &actual));
    SQL("COMMIT");
    int resolver = openat(network_scope.descriptors[1], "journal-resolv.conf",
                          O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    const char bytes[] = "nameserver 10.0.2.3\noptions timeout:2 attempts:2\n";
    CHECK(resolver >= 3 && write(resolver, bytes, sizeof(bytes) - 1) == sizeof(bytes) - 1);
    close(resolver);
    resolver = openat(network_scope.descriptors[1], "journal-resolv.conf",
                       O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(resolver >= 3);
    for (unsigned i = 0; i < 2; i++) {
        const char *component = i ? "oidc-kratos" : "oidc-hydra";
        char id[65];
        int length = snprintf(id, sizeof(id), "migrate-%s-%s", component, target->operation_id);
        CHECK(length > 0 && length < (int)sizeof(id));
        CHECK(!scope(env, target, id, "workload", &scopes[i]));
        if (native_source) {
            char path[4096];
            int n = snprintf(path, sizeof(path), "%s/%s/%s", native_source,
                             target->operation_id, i ? "kratos" : "hydra");
            CHECK(n > 0 && n < (int)sizeof(path));
            int materialized = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            CHECK(materialized >= 3);
            CHECK(!renameat(materialized, "rootfs", scopes[i].descriptors[1], "rootfs"));
            close(materialized);
        } else
            CHECK(!mkdirat(scopes[i].descriptors[1], "rootfs", 0700));
        int rootfs = openat(scopes[i].descriptors[1], "rootfs",
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        CHECK(rootfs >= 3);
        SQL("BEGIN IMMEDIATE");
        CHECK(!gstd_deployment_execution_record(db, target, id, &scopes[i], rootfs));
        SQL("COMMIT");
        gstd_execution_inputs input = {.manifest = target->manifest,
                                        .component = component,
                                        .purpose = GSTD_EXEC_OIDC_MIGRATE,
                                        .rootfs_fd = rootfs,
                                        .network = &actual.network,
                                        .oidc_ports = &setup->ports};
        for (unsigned j = 0; j < GSTD_MOUNT_COUNT; j++)
            input.role_fds[j] = -1;
        input.role_fds[GSTD_MOUNT_RESOLVER] = resolver;
        input.role_fds[GSTD_MOUNT_DATASTORE] = setup->volumes[i].data_fd;
        json_t *plan = gstd_execution_build(&input);
        CHECK(plan);
        const json_t *limits = json_object_get(json_object_get(plan, "process"), "rlimits");
#ifdef GITSTORED_INSTRUMENTED
        CHECK(json_array_size(limits) == 1);
#else
        CHECK(json_array_size(limits) == 2 &&
              json_integer_value(json_object_get(json_array_get(limits, 0), "hard")) ==
                  (json_int_t)(UINT64_C(1) << 40));
#endif
        CHECK(json_integer_value(json_object_get(json_object_get(json_object_get(
                  json_object_get(plan, "linux"), "resources"), "memory"), "limit")) == 536870912);
        gstd_native_context context = {.backend_id = id,
                                        .component = component,
                                        .boot_id = actual.network.boot_id,
                                        .state_fd = native_source ? env->native_fd : env->resource_fd,
                                        .bundle_fd = scopes[i].descriptors[2],
                                        .rootfs_fd = rootfs,
                                        .stdout_fd = -1,
                                        .stderr_fd = -1,
                                        .writer_lease_fd = setup->volumes[i].writer_fd,
                                        .control_owner = getpid(),
                                        .configuration = plan,
                                        .registered_inputs = &input};
        SQL("BEGIN IMMEDIATE");
        /* Metadata or a free scope cannot replace the committed attachment. */
        CHECK(gstd_deployment_oidc_helper_reserve(db, target, setup->configuration_id,
                                                   &context) == -1);
        CHECK(!gstd_network_attachment_reserve(db, &owner, &actual, id));
        gstd_native_context wrong = context;
        wrong.writer_lease_fd = setup->volumes[1 - i].writer_fd;
        CHECK(gstd_deployment_oidc_helper_reserve(db, target, setup->configuration_id, &wrong) == -1);
        int reserved = gstd_deployment_oidc_helper_reserve(db, target, setup->configuration_id,
                                                            &context);
        CHECK(expect_dispatch ? reserved == 0 : reserved == -1);
        SQL("ROLLBACK");
        SQL("BEGIN IMMEDIATE");
        CHECK(!gstd_network_attachment_reserve(db, &owner, &actual, id));
        if (!native_source) {
            reserved = gstd_deployment_oidc_helper_reserve(db, target, setup->configuration_id,
                                                          &context);
            CHECK(expect_dispatch ? reserved == 0 : reserved == -1);
        }
        SQL("COMMIT");
        if (native_source) {
            int log = openat(network_scope.descriptors[1], i ? "kratos-migration.log" :
                             "hydra-migration.log",
                             O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
            CHECK(log >= 3);
            context.stdout_fd = context.stderr_fd = log;
            gstd_helper_owner migration = {0};
            native_may_exist = 1;
            CHECK(!gstd_helper_owner_begin(db, target, setup->configuration_id, &context, &migration));
            int complete = 0;
            while (!(complete = gstd_helper_owner_poll(&migration, -1)))
                usleep(1000);
            if (complete != 1 || migration.outcome || !migration.helper.deleted ||
                migration.helper.may_exist)
                fprintf(stderr, "OIDC migration complete=%d outcome=%d stage=%d nativeError=%d "
                                "pid=%ld deleted=%d mayExist=%d\n", complete, migration.outcome,
                        migration.helper.stage, migration.helper.observed.native_error,
                        (long)migration.helper.context.expected_pid, migration.helper.deleted,
                        migration.helper.may_exist);
            CHECK(complete == 1 && !migration.outcome && migration.helper.deleted &&
                  !migration.helper.may_exist);
            native_may_exist = 0;
            CHECK(!gstd_helper_owner_close(&migration));
            close(log);
        }
        SQL("BEGIN IMMEDIATE");
        CHECK(gstd_deployment_oidc_helper_reserve(db, target, setup->configuration_id, &context) == -1);
        CHECK(gstd_deployment_helper_reserve(db, target, setup->configuration_id, id,
                                              GSTD_EXEC_OIDC_MIGRATE) == -1);
        SQL("ROLLBACK");
        json_decref(plan);
        close(rootfs);
    }
    close(resolver);
    CHECK(!gstd_network_worker_signal(&owner, 0));
    struct pollfd exited = {.fd = owner.pid_fd, .events = POLLIN};
    CHECK(poll(&exited, 1, 10000) == 1);
    int status;
    CHECK(waitpid(owner.pid, &status, 0) == owner.pid && WIFEXITED(status) && !WEXITSTATUS(status));
    gstd_network_worker_close(&owner);
    cleanup_owner = (gstd_network_worker)GSTD_NETWORK_WORKER_INITIAL;
    gstd_filesystem_reply_close(&network_scope);
    for (unsigned i = 0; i < 2; i++)
        gstd_filesystem_reply_close(&scopes[i]);
    return 0;
}
