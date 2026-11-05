#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "cleanup.h"
#include "deployment_records.h"
#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "deployment records line %d: %s\n", __LINE__, #x);                     \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#define SQL(x) CHECK(sqlite3_exec(db, x, NULL, NULL, NULL) == SQLITE_OK)
int main(void) {
    gstd_json_initialize();
    char *root = gstd_test_dir(), path[4096], op[65];
    CHECK(root);
    snprintf(path, sizeof(path), "%s/state.db", root);
    sqlite3 *db = NULL;
    CHECK(!gstd_state_open(path, &db));
    json_t *components = json_pack("[s,s,s]", "api", "git", "controller");
#if defined(__aarch64__) || defined(__arm64__)
    const char *architecture = "arm64", *platform = "linux-arm64";
#else
    const char *architecture = "amd64", *platform = "linux-amd64";
#endif
    json_t *manifest = gstd_asset_manifest(components, architecture);
    CHECK(manifest);
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(db, "INSERT INTO profiles VALUES('core','1',?,?,?)", -1, &s, NULL) ==
          SQLITE_OK);
    sqlite3_bind_text(s, 1, json_string_value(json_object_get(manifest, "serverRevision")), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, json_string_value(json_object_get(manifest, "profileDigest")), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, platform, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    const char text[] =
        "{\"version\":{\"major\":1,\"minor\":0},\"requestId\":\"r\",\"method\":\"deployment."
        "start\",\"idempotencyKey\":\"deployment-fixture\",\"params\":{\"deploymentId\":\"one\","
        "\"profileId\":\"core\",\"profileVersion\":\"1\",\"components\":[\"controller\"]}}";
    json_t *message = gstd_json_parse((const unsigned char *)text, sizeof(text) - 1);
    gstd_request request;
    CHECK(message && !gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, components, "repositories", op) == GSTD_ACCEPT_NEW);
    char config[65];
    CHECK(snprintf(config, sizeof(config), "cfg-%s", op) > 0);
    gstd_deployment_target target = {0}, absent = {0};
    CHECK(gstd_deployment_target_read(db, "absent", &absent) < 0);
    CHECK(!gstd_deployment_target_read(db, op, &target));
    CHECK(target.generation == 1 && !target.allow_provision &&
          json_array_size(target.components) == 3);
    CHECK(gstd_deployment_resource_reserve(db, &target, config, "configuration") < 0);
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_deployment_resource_reserve(db, &target, config, "configuration"));
    CHECK(gstd_deployment_resource_reserve(db, &target, config, "configuration") < 0);
    CHECK(gstd_deployment_resource_reserve(db, &target, "foreign", "repository-volume") < 0);
    CHECK(!gstd_deployment_resource_reserve(db, &target, "network", "network"));
    SQL("COMMIT");
    int parent = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(parent >= 3);
    CHECK(!mkdirat(parent, "scope", 0700));
    int scope = openat(parent, "scope", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(scope >= 3);
    CHECK(!mkdirat(scope, "data", 0700));
    int data = openat(scope, "data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(data >= 3);
    struct stat info;
    CHECK(!fstat(scope, &info));
    gstd_filesystem_reply receipt = {.descriptor_count = 2,
                                     .descriptors = {scope, data},
                                     .scope_device = (uint64_t)info.st_dev,
                                     .scope_inode = (uint64_t)info.st_ino};
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_deployment_scope_record(db, &target, "network", &receipt) < 0);
    CHECK(!gstd_deployment_scope_record(db, &target, config, &receipt));
    CHECK(gstd_deployment_scope_record(db, &target, config, &receipt) < 0);
    gstd_bootstrap_graph graph = {0};
    CHECK(!gstd_deployment_graph_begin(db, &target, config, &graph));
    CHECK(gstd_deployment_graph_begin(db, &target, config, &graph) < 0);
    int initial = -1;
    CHECK(!gstd_deployment_credential_reserve(db, &target, config, &initial) && initial == 1);
    SQL("ROLLBACK");
    /* A rolled-back reservation grants no worker effect and can be retried. */
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_deployment_scope_record(db, &target, config, &receipt));
    CHECK(!gstd_deployment_graph_begin(db, &target, config, &graph));
    CHECK(!gstd_deployment_credential_reserve(db, &target, config, &initial) && initial == 1);
    SQL("COMMIT");
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_deployment_credential_reserve(db, &target, config, &initial) && initial == 0);
    CHECK(gstd_deployment_graph_step(db, NULL, config, &graph, GSTD_BOOT_PREPARE, 0, NULL) < 0);
    gstd_bootstrap_graph stale = graph;
    stale.generation++;
    CHECK(gstd_deployment_graph_step(db, &target, config, &stale, GSTD_BOOT_PREPARE, 0, NULL) < 0);
    stale = graph;
    stale.persistent = 1;
    CHECK(gstd_deployment_graph_step(db, &target, config, &stale, GSTD_BOOT_PREPARE, 0, NULL) < 0);
    stale = graph;
    stale.fresh_datastore = 1;
    CHECK(gstd_deployment_graph_step(db, &target, config, &stale, GSTD_BOOT_PREPARE, 0, NULL) < 0);
    CHECK(!gstd_deployment_graph_step(db, &target, config, &graph, GSTD_BOOT_PREPARE, 0, NULL));
    CHECK(graph.stage == GSTD_BOOT_LAUNCH_GIT);
    SQL("COMMIT");
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_deployment_credential_reserve(db, &target, config, &initial) < 0);
    CHECK(gstd_deployment_graph_step(db, &target, config, &graph, GSTD_BOOT_PREPARE, 0, NULL) < 0);
    /* Public controller UID survives an uncertain enrollment outcome. */
    const gstd_bootstrap_stage stages[] = {GSTD_BOOT_LAUNCH_GIT, GSTD_BOOT_LAUNCH_API,
                                           GSTD_BOOT_WAIT_API, GSTD_BOOT_WAIT_GIT};
    for (unsigned i = 0; i < sizeof(stages) / sizeof(stages[0]); i++)
        CHECK(!gstd_deployment_graph_step(db, &target, config, &graph, stages[i], 0, NULL));
    CHECK(!gstd_deployment_graph_step(db, &target, config, &graph, GSTD_BOOT_ENROLL, -2,
                                      "known-controller"));
    CHECK(graph.outcome == -2 && !strcmp(graph.controller_uid, "known-controller"));
    SQL("COMMIT");
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_deployment_graph_step(db, &target, config, &graph, GSTD_BOOT_ENROLL, 0,
                                     "known-controller") < 0);
    SQL("UPDATE deployments SET generation=2 WHERE deployment_id='one'");
    gstd_deployment_target recovered = {0};
    CHECK(!gstd_deployment_target_recovery_read(db, op, &recovered));
    CHECK(recovered.generation == 1 && !recovered.allow_provision &&
          !strcmp(recovered.operation_id, op));
    gstd_deployment_target_clear(&recovered);
    SQL("UPDATE resource_ownership SET ownership_state='released' WHERE "
        "resource_type='configuration'");
    CHECK(gstd_deployment_target_recovery_read(db, op, &recovered) < 0);

    CHECK(gstd_deployment_resource_reserve(db, &target, "stale", "configuration") < 0);
    SQL("ROLLBACK");
    /* Exact materialized execution receipts precede native side effects. */
    CHECK(!mkdirat(parent, "workload", 0700));
    int work = openat(parent, "workload", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(work >= 3 && !mkdirat(work, "execution", 0700) && !mkdirat(work, "bundle", 0700));
    int execution = openat(work, "execution", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int bundle = openat(work, "bundle", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(execution >= 3 && bundle >= 3 && !mkdirat(execution, "rootfs", 0755));
    int rootfs = openat(execution, "rootfs", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(rootfs >= 3 && !fstat(work, &info));
    gstd_filesystem_reply work_receipt = {.descriptor_count = 3,
                                          .descriptors = {work, execution, bundle},
                                          .scope_device = (uint64_t)info.st_dev,
                                          .scope_inode = (uint64_t)info.st_ino};
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_deployment_resource_reserve(db, &target, "workload", "workload"));
    CHECK(!gstd_deployment_scope_record(db, &target, "workload", &work_receipt));
    CHECK(gstd_deployment_execution_record(db, &target, "workload", &work_receipt, data) < 0);
    CHECK(!gstd_deployment_execution_record(db, &target, "workload", &work_receipt, rootfs));
    CHECK(!gstd_deployment_execution_record(db, &target, "workload", &work_receipt, rootfs));
    SQL("COMMIT");
    CHECK(!renameat(execution, "rootfs", execution, "retained-rootfs") &&
          !mkdirat(execution, "rootfs", 0755));
    int replacement = openat(execution, "rootfs", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(replacement >= 3);
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_deployment_execution_record(db, &target, "workload", &work_receipt, rootfs) < 0);
    CHECK(gstd_deployment_execution_record(db, &target, "workload", &work_receipt, replacement) <
          0);
    SQL("ROLLBACK");
#ifdef __linux__
    /* Finish the synthetic start before accepting its separate stop. */
    char scheduled[65];
    CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(scheduled, op));
    SQL("UPDATE operations SET state='failed',error='{\"code\":\"CONTROLLED_FAILURE\"}'");
    CHECK(!gstd_schedule_finish(db, op));
    /* Metadata preparation alone must not authorize stopping/releasing resources. */
    json_t *stop_message =
        json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s}}", "version", "major", 1, "minor", 0,
                  "requestId", "stop", "method", "deployment.stop", "idempotencyKey",
                  "stop-unproven", "params", "deploymentId", "one");
    gstd_request stop_request;
    char stop_operation[65];
    CHECK(stop_message && !gstd_request_parse(stop_message, &stop_request));
    CHECK(gstd_accept(db, &stop_request, NULL, NULL, stop_operation) == GSTD_ACCEPT_NEW);
    gstd_runtime_settings settings = GSTD_RUNTIME_SETTINGS_INITIAL;
    gstd_deployment_environment environment = {.database = db,
                                               .settings = &settings,
                                               .cache_fd = parent,
                                               .resource_fd = parent,
                                               .native_fd = parent,
                                               .broker_fd = -1};
    gstd_deployment prepared = {0};
    CHECK(!gstd_deployment_recovery_prepare(&environment, op, &prepared));
    gstd_cleanup cleanup = {0};
    CHECK(prepared.recovered && !prepared.recovery_proven && !prepared.target.allow_provision);
    CHECK(gstd_cleanup_begin(&prepared, stop_operation, &cleanup) == -1 && !cleanup.stage);
    CHECK(sqlite3_prepare_v2(
              db, "SELECT generation,desired_state FROM deployments WHERE deployment_id='one'", -1,
              &s, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int64(s, 0) == 1 &&
          !strcmp((const char *)sqlite3_column_text(s, 1), "running"));
    sqlite3_finalize(s);
    /* Controlled metadata model only: no native handles/workloads are present.
     * Force both deadlines before any filesystem/native dispatch. Real paused
     * workload retry is exercised separately by tests/runtime/test_recovery.py. */
    CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(scheduled, stop_operation));
    prepared.recovery_proven = 1;
    CHECK(!gstd_cleanup_begin(&prepared, stop_operation, &cleanup));
    CHECK(cleanup.generation == 2 && prepared.target.generation == 1);
    CHECK(gstd_cleanup_retry_begin(&cleanup, stop_operation) == -1);
    cleanup.deadline_ms = 0;
    CHECK(gstd_cleanup_poll(&cleanup) == 1 && cleanup.outcome == -2);
    CHECK(!gstd_schedule_finish(db, stop_operation));
    json_object_set_new(stop_message, "idempotencyKey", json_string("retry-stop"));
    CHECK(!gstd_request_parse(stop_message, &stop_request));
    char retried[65];
    CHECK(gstd_accept(db, &stop_request, NULL, NULL, retried) == GSTD_ACCEPT_NEW);
    CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(scheduled, retried));
    CHECK(!gstd_cleanup_retry_begin(&cleanup, retried));
    CHECK(cleanup.generation == 3 && prepared.target.generation == 1 &&
          prepared.recovery_generation == 3 && !strcmp(prepared.recovery_operation, retried));
    cleanup.deadline_ms = 0;
    CHECK(gstd_cleanup_poll(&cleanup) == 1 && cleanup.outcome == -2);
    CHECK(!gstd_schedule_finish(db, retried));
    CHECK(sqlite3_prepare_v2(
              db,
              "SELECT count(*) FROM operations WHERE method='deployment.stop' "
              "AND state='failed' AND json_extract(error,'$.code')='OWNERSHIP_UNCERTAIN'",
              -1, &s, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 2);
    sqlite3_finalize(s);
    gstd_deployment_target_clear(&prepared.target);
    json_decref(stop_message);
#endif
    close(replacement);
    close(rootfs);
    close(bundle);
    close(execution);
    close(work);
    close(data);
    close(scope);
    close(parent);
    gstd_deployment_target_clear(&target);
    json_decref(message);
    json_decref(manifest);
    json_decref(components);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    CHECK(!gstd_test_remove(root));
    return 0;
}
