#define _GNU_SOURCE 1
#include "datastore.h"
#include "datastore_start.h"
#include "deployment_records.h"
#include "filesystem_job.h"
#include "gitstored/asset_records.h"
#include "gitstored/asset_tree.h"
#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "native_exit.h"
#include "support.h"
#include "volume_records.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "filesystem job line %d\n", __LINE__);                                 \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int run(const gstd_filesystem_input *input, gstd_filesystem_reply *reply) {
    gstd_filesystem_job job = {0};
    if (gstd_filesystem_launch(input, 5000, &job))
        return -1;
    for (unsigned n = 0; n < 6000; n++) {
        int ready = gstd_filesystem_poll(&job, -1, reply);
        if (ready)
            return ready == 1 ? 0 : -1;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    return -1;
}
static int reserve(sqlite3 *db, const char *id, const char *kind, const char *operation) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO resource_ownership "
                           "SELECT "
                           "?,?,deployment_id,operation_id,'reserved',NULL,'held',strftime('%Y-%m-%"
                           "dT%H:%M:%SZ','now') FROM operations WHERE operation_id=?",
                           -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, operation, -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(s);
    return ok ? 0 : -1;
}
int main(void) {
    alarm(20);
    CHECK(geteuid() != 0);
    gstd_json_initialize();
    char *path = gstd_test_dir();
    CHECK(path);
    int root = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(root >= 3);
    char filename[4096];
    snprintf(filename, sizeof(filename), "%s/state.db", path);
    sqlite3 *db = NULL;
    CHECK(!gstd_state_open(filename, &db));
    json_t *components = json_pack("[s,s]", "api", "git");
#if defined(__aarch64__)
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
        "start\",\"idempotencyKey\":\"filesystem-fixture\",\"params\":{\"deploymentId\":\"one\","
        "\"profileId\":\"core\",\"profileVersion\":\"1\"}}";
    json_t *message = gstd_json_parse((const unsigned char *)text, sizeof(text) - 1);
    gstd_request request;
    char operation[65];
    CHECK(message && !gstd_request_parse(message, &request) &&
          gstd_accept(db, &request, components, "repositories", operation) == GSTD_ACCEPT_NEW);
    CHECK(!reserve(db, "configuration-fixture", "configuration", operation));
    CHECK(!reserve(db, "workload-fixture", "workload", operation));
    gstd_cache_owner expected;
    CHECK(gstd_asset_record_cache_state(db, &expected) == 1);
    gstd_filesystem_input input = {.action = GSTD_FS_INITIALIZE,
                                   .store_id = expected.store_id,
                                   .descriptors = {root, -1, -1},
                                   .expected_cache = expected};
    gstd_filesystem_reply initialized = {0};
    CHECK(!run(&input, &initialized) && initialized.status == 0 &&
          initialized.descriptor_count == 6);
    for (unsigned i = 0; i < 6; i++)
        CHECK(initialized.descriptors[i] >= 3 &&
              (fcntl(initialized.descriptors[i], F_GETFD) & FD_CLOEXEC));
    CHECK(initialized.cache_owner.lock_fd == -1 && initialized.volume_root.root_fd == -1 &&
          initialized.volume_root.lock_fd == -1);
    gstd_cache_owner cache_owner = initialized.cache_owner;
    cache_owner.lock_fd = initialized.descriptors[1];
    gstd_volume_root volume_owner = initialized.volume_root;
    volume_owner.root_fd = initialized.descriptors[2];
    volume_owner.lock_fd = initialized.descriptors[3];
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK &&
          !gstd_asset_record_cache_bind(db, &cache_owner) &&
          sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    const char *stage_key = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    input = (gstd_filesystem_input){.action = GSTD_FS_STAGE_DIRECTORY,
                                    .asset_key = stage_key,
                                    .operation_id = operation,
                                    .descriptors = {initialized.descriptors[0], -1, -1}};
    gstd_filesystem_reply staging = {0}, duplicate_stage = {0};
    CHECK(!run(&input, &staging) && !staging.status && staging.descriptor_count == 1);
    CHECK(fcntl(staging.descriptors[0], F_GETFD) & FD_CLOEXEC);
    struct stat stage_info;
    CHECK(!fstat(staging.descriptors[0], &stage_info) &&
          (uint64_t)stage_info.st_ino == staging.scope_inode);
    CHECK(!run(&input, &duplicate_stage) && duplicate_stage.status == -1 &&
          !duplicate_stage.descriptor_count);
    input.asset_key = "../escape";
    CHECK(!run(&input, &duplicate_stage) && duplicate_stage.status == -1);
    gstd_filesystem_reply_close(&staging);
    input = (gstd_filesystem_input){.action = GSTD_FS_SCOPE,
                                    .store_id = expected.store_id,
                                    .operation_id = operation,
                                    .deployment_id = "one",
                                    .generation = 1,
                                    .resource_id = "configuration-fixture",
                                    .resource_kind = "configuration",
                                    .descriptors = {initialized.descriptors[4], -1, -1}};
    gstd_filesystem_reply scope = {0}, again = {0};
    CHECK(!run(&input, &scope) && scope.status == 0 && scope.descriptor_count == 2);
    CHECK(!run(&input, &again) && again.status == -1 && !again.descriptor_count);
    input.resource_id = "workload-fixture";
    input.resource_kind = "workload";
    gstd_filesystem_reply workload = {0};
    CHECK(!run(&input, &workload) && workload.status == 0 && workload.descriptor_count == 3);
    input.resource_id = "../escape";
    CHECK(!run(&input, &again) && again.status == -1);
    gstd_deployment_target target = {0};
    CHECK(!gstd_deployment_target_read(db, operation, &target));
    CHECK(!mkdirat(workload.descriptors[1], "rootfs", 0755));
    int rootfs =
        openat(workload.descriptors[1], "rootfs", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(rootfs >= 3);
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!gstd_deployment_scope_record(db, &target, "configuration-fixture", &scope));
    CHECK(!gstd_deployment_scope_record(db, &target, "workload-fixture", &workload));
    CHECK(!gstd_deployment_execution_record(db, &target, "workload-fixture", &workload, rootfs));
    CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    for (unsigned kind = 0; kind < 2; kind++) {
        const char *id = kind ? "workload-fixture" : "configuration-fixture";
        CHECK(sqlite3_prepare_v2(
                  db, "SELECT backend_identity FROM resource_ownership WHERE resource_id=?", -1, &s,
                  NULL) == SQLITE_OK);
        sqlite3_bind_text(s, 1, id, -1, SQLITE_STATIC);
        CHECK(sqlite3_step(s) == SQLITE_ROW);
        json_t *receipt =
            gstd_json_parse(sqlite3_column_text(s, 0), (size_t)sqlite3_column_bytes(s, 0));
        sqlite3_finalize(s);
        CHECK(receipt);
        input = (gstd_filesystem_input){.action = GSTD_FS_REOPEN_SCOPE,
                                        .manifest = manifest,
                                        .scope_expected = receipt,
                                        .store_id = target.store_id,
                                        .operation_id = operation,
                                        .deployment_id = "one",
                                        .generation = 1,
                                        .resource_id = id,
                                        .resource_kind = kind ? "workload" : "configuration",
                                        .descriptors = {initialized.descriptors[4], -1, -1}};
        CHECK(!run(&input, &again) && !again.status && again.descriptor_count == (kind ? 4u : 2u));
        struct stat before, after;
        CHECK(!fstat(kind ? workload.descriptors[0] : scope.descriptors[0], &before) &&
              !fstat(again.descriptors[0], &after) && before.st_dev == after.st_dev &&
              before.st_ino == after.st_ino);
        gstd_filesystem_reply_close(&again);
        input.generation = 2;
        CHECK(!run(&input, &again) && again.status == -1 && !again.descriptor_count);
        input.generation = 1;
        json_t *inode = json_incref(json_object_get(receipt, "scopeInode"));
        CHECK(!json_object_set_new(receipt, "scopeInode", json_string("1")));
        CHECK(!run(&input, &again) && again.status == -1 && !again.descriptor_count);
        CHECK(!json_object_set(receipt, "scopeInode", inode));
        json_decref(inode);
        if (kind) {
            CHECK(!renameat(workload.descriptors[1], "rootfs", workload.descriptors[1],
                            "saved-rootfs"));
            CHECK(!run(&input, &again) && again.status == -1 && !again.descriptor_count);
            CHECK(faccessat(workload.descriptors[1], "rootfs", F_OK, AT_SYMLINK_NOFOLLOW) < 0);
            CHECK(!mkdirat(workload.descriptors[1], "rootfs", 0755));
            CHECK(!run(&input, &again) && again.status == -1 && !again.descriptor_count);
            CHECK(!unlinkat(workload.descriptors[1], "rootfs", AT_REMOVEDIR));
            CHECK(!renameat(workload.descriptors[1], "saved-rootfs", workload.descriptors[1],
                            "rootfs"));
        }
        json_decref(receipt);
    }
    CHECK(!reserve(db, "network-scope-fixture", "network", operation));
    input = (gstd_filesystem_input){.action = GSTD_FS_SCOPE,
                                    .store_id = target.store_id,
                                    .operation_id = operation,
                                    .deployment_id = "one",
                                    .generation = 1,
                                    .resource_id = "network-scope-fixture",
                                    .resource_kind = "network",
                                    .descriptors = {initialized.descriptors[4], -1, -1}};
    gstd_filesystem_reply network_scope = {0};
    CHECK(!run(&input, &network_scope) && !network_scope.status &&
          network_scope.descriptor_count == 2);
    struct stat net_info;
    CHECK(!fstat(network_scope.descriptors[1], &net_info));
    gstd_network_worker synthetic = GSTD_NETWORK_WORKER_INITIAL;
    synthetic.pid = getpid();
    synthetic.roles = 3;
    CHECK(!gstd_native_process_start(getpid(), &synthetic.start) &&
          !gstd_native_boot_id(synthetic.boot_id));
    snprintf(synthetic.store_id, sizeof(synthetic.store_id), "%s", target.store_id);
    snprintf(synthetic.resource_id, sizeof(synthetic.resource_id), "network-scope-fixture");
    snprintf(synthetic.operation_id, sizeof(synthetic.operation_id), "%s", operation);
    snprintf(synthetic.profile_digest, sizeof(synthetic.profile_digest), "%s",
             json_string_value(json_object_get(manifest, "profileDigest")));
    synthetic.scope_device = net_info.st_dev;
    synthetic.scope_inode = net_info.st_ino;
    json_t *network_identity = gstd_network_worker_identity(&synthetic);
    CHECK(network_identity);
    input.action = GSTD_FS_REOPEN_SCOPE;
    input.manifest = manifest;
    input.scope_expected = network_identity;
    CHECK(!run(&input, &again) && !again.status && again.descriptor_count == 1 &&
          again.scope_inode == (uint64_t)net_info.st_ino);
    int recovered_scope = again.descriptors[0];
    again.descriptor_count = 0;
    input.action = GSTD_FS_RECOVER_NETWORK;
    input.descriptors[0] = recovered_scope;
    CHECK(!run(&input, &again) && again.status == -1 && !again.descriptor_count);
    /* A bound directory and ordinary process identity grant no network control. */
    close(recovered_scope);
    json_decref(network_identity);
    gstd_filesystem_reply_close(&network_scope);
    close(rootfs);
    gstd_deployment_target_clear(&target);

    gstd_volume_identity volume_expected;
    char producer[65];
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK &&
          gstd_volume_record_begin(db, &volume_owner, "repositories", operation, producer,
                                   &volume_expected) == 0 &&
          sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    input = (gstd_filesystem_input){.action = GSTD_FS_VOLUME,
                                    .descriptors = {volume_owner.root_fd, volume_owner.lock_fd, -1},
                                    .volume_root = volume_owner,
                                    .volume_id = "repositories",
                                    .producer_id = producer};
    gstd_filesystem_reply volume = {0};
    CHECK(!run(&input, &volume) && volume.status == 0 && volume.descriptor_count == 2);
    gstd_volume_handle handle = {.data_fd = volume.descriptors[0],
                                 .writer_fd = volume.descriptors[1],
                                 .identity = volume.volume};
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK &&
          !gstd_volume_record_ready(db, &volume_owner, "repositories", operation, &handle) &&
          sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    input.volume_existing = 1;
    input.volume_expected = handle.identity;
    CHECK(!run(&input, &again) && again.status == -1 && again.detail == GSTD_VOLUME_HELD &&
          !again.descriptor_count);
    input.action = GSTD_FS_OBSERVE_VOLUME;
    CHECK(!run(&input, &again) && !again.status && again.descriptor_count == 1 &&
          again.volume.data_inode == input.volume_expected.data_inode);
    gstd_filesystem_reply_close(&again);
    input.action = GSTD_FS_VOLUME;
    CHECK(!run(&input, &again) && again.status == -1 && again.detail == GSTD_VOLUME_HELD &&
          !again.descriptor_count);
    int data = openat(handle.data_fd, "retained", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(data >= 3 && write(data, "retained", 8) == 8 && !close(data));
    gstd_filesystem_reply_close(&volume);
    CHECK(!run(&input, &volume) && volume.status == 0 &&
          volume.volume.data_inode == input.volume_expected.data_inode);
    data = openat(volume.descriptors[0], "retained", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    char bytes[16];
    CHECK(data >= 3 && read(data, bytes, sizeof(bytes)) == 8 && !memcmp(bytes, "retained", 8) &&
          !close(data));
    json_t *datastore_manifest =
        gstd_asset_manifest_for_datastore(components, architecture, "scylladb");
    json_t *datastore_message =
        json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:s,s:s}}", "version", "major", 1, "minor",
                  0, "requestId", "datastore", "method", "deployment.start", "idempotencyKey",
                  "datastore-worker", "params", "deploymentId", "persistent", "profileId", "core",
                  "profileVersion", "1", "datastore", "scylladb");
    gstd_request datastore_request;
    char datastore_operation[65], datastore_producer[65], datastore_id[65];
    CHECK(datastore_manifest && datastore_message &&
          !gstd_request_parse(datastore_message, &datastore_request));
    CHECK(gstd_accept(db, &datastore_request, components, NULL, datastore_operation) ==
          GSTD_ACCEPT_NEW);
    gstd_deployment_target persistent_target = {0};
    CHECK(!gstd_deployment_target_read(db, datastore_operation, &persistent_target));
    CHECK(json_equal(persistent_target.manifest, datastore_manifest));
    CHECK(!reserve(db, "datastore-configuration", "configuration", datastore_operation));
    input = (gstd_filesystem_input){.action = GSTD_FS_SCOPE,
                                    .store_id = expected.store_id,
                                    .operation_id = datastore_operation,
                                    .deployment_id = "persistent",
                                    .generation = 1,
                                    .resource_id = "datastore-configuration",
                                    .resource_kind = "configuration",
                                    .descriptors = {initialized.descriptors[4], -1, -1}};
    gstd_filesystem_reply datastore_configuration = {0};
    CHECK(!run(&input, &datastore_configuration) && !datastore_configuration.status);
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK &&
          !gstd_deployment_scope_record(db, &persistent_target, "datastore-configuration",
                                        &datastore_configuration) &&
          sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    gstd_datastore_start prefix = {0}, refused_prefix = {0};
    CHECK(!gstd_datastore_start_begin(db, &persistent_target, &volume_owner,
                                      "datastore-configuration", &prefix));
    CHECK(prefix.graph.persistent && prefix.graph.fresh_datastore && prefix.worker.worker &&
          sqlite3_get_autocommit(db));
    CHECK(gstd_datastore_start_begin(db, &persistent_target, &volume_owner,
                                     "datastore-configuration", &refused_prefix) == -1 &&
          !refused_prefix.worker.worker && sqlite3_get_autocommit(db));
    int prefix_ready = 0;
    for (unsigned n = 0; n < 6000 && !prefix_ready; n++) {
        prefix_ready = gstd_datastore_start_poll(&prefix, -1);
        if (!prefix_ready) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
    }
    CHECK(prefix_ready == 1 && !prefix.outcome && prefix.volume.data_fd >= 3 &&
          prefix.volume.writer_fd >= 3);
    memcpy(datastore_id, prefix.volume_id, sizeof(datastore_id));
    memcpy(datastore_producer, prefix.producer_id, sizeof(datastore_producer));
    gstd_filesystem_reply datastore = {
        .descriptor_count = 2,
        .descriptors = {prefix.volume.data_fd, prefix.volume.writer_fd},
        .volume = prefix.volume.identity};
    prefix.volume.data_fd = prefix.volume.writer_fd = -1;
    CHECK(!gstd_datastore_start_close(&prefix));
    input = (gstd_filesystem_input){.action = GSTD_FS_DATASTORE_VOLUME,
                                    .manifest = manifest,
                                    .descriptors = {volume_owner.root_fd, volume_owner.lock_fd, -1},
                                    .volume_root = volume_owner,
                                    .volume_id = datastore_id,
                                    .producer_id = datastore_producer};
    gstd_filesystem_job refused = {0};
    CHECK(gstd_filesystem_launch(&input, 5000, &refused) == -1 && !refused.worker);
    input.manifest = datastore_manifest;
    gstd_volume_handle datastore_handle = {.data_fd = datastore.descriptors[0],
                                           .writer_fd = datastore.descriptors[1],
                                           .identity = datastore.volume};
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK &&
          !gstd_scylla_volume_record_ready(db, &volume_owner, datastore_id, datastore_operation,
                                           &datastore_handle) &&
          sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    input.volume_existing = 1;
    input.volume_expected = datastore.volume;
    CHECK(!run(&input, &again) && again.status == -1 && again.detail == GSTD_VOLUME_HELD &&
          !again.descriptor_count);
    input.action = GSTD_FS_OBSERVE_DATASTORE;
    CHECK(!run(&input, &again) && !again.status && again.descriptor_count == 1 &&
          again.volume.data_inode == input.volume_expected.data_inode);
    gstd_filesystem_reply_close(&again);
    data =
        openat(datastore_handle.data_fd, "retained", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(data >= 3 && write(data, "scylla-retained", 15) == 15 && !close(data));
    gstd_filesystem_reply_close(&datastore);
    gstd_filesystem_reply_close(&datastore_configuration);
    gstd_deployment_target_clear(&persistent_target);
    input.action = GSTD_FS_DATASTORE_VOLUME;
    CHECK(!run(&input, &datastore) && !datastore.status && datastore.descriptor_count == 2);
    data = openat(datastore.descriptors[0], "retained", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(data >= 3 && read(data, bytes, sizeof(bytes)) == 15 &&
          !memcmp(bytes, "scylla-retained", 15) && !close(data));
    gstd_filesystem_reply_close(&datastore);
    json_decref(datastore_manifest);
    json_decref(datastore_message);
    /* Actual asynchronous persistent preparation with consent withheld. It
     * must reserve/retain owned state, then fail before any image transfer. */
    datastore_message =
        json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:s,s:s}}", "version", "major", 1, "minor",
                  0, "requestId", "preparation", "method", "deployment.start", "idempotencyKey",
                  "datastore-preparation", "params", "deploymentId", "preparation", "profileId",
                  "core", "profileVersion", "1", "datastore", "scylladb");
    CHECK(datastore_message && !gstd_request_parse(datastore_message, &datastore_request));
    CHECK(gstd_accept(db, &datastore_request, components, NULL, datastore_operation) ==
          GSTD_ACCEPT_NEW);
    CHECK(!gstd_deployment_target_read(db, datastore_operation, &persistent_target));
    CHECK(!reserve(db, "preparation-configuration", "configuration", datastore_operation));
    input = (gstd_filesystem_input){.action = GSTD_FS_SCOPE,
                                    .store_id = expected.store_id,
                                    .operation_id = datastore_operation,
                                    .deployment_id = "preparation",
                                    .generation = 1,
                                    .resource_id = "preparation-configuration",
                                    .resource_kind = "configuration",
                                    .descriptors = {initialized.descriptors[4], -1, -1}};
    CHECK(!run(&input, &datastore_configuration) && !datastore_configuration.status);
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK &&
          !gstd_deployment_scope_record(db, &persistent_target, "preparation-configuration",
                                        &datastore_configuration) &&
          sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    gstd_runtime_settings settings = GSTD_RUNTIME_SETTINGS_INITIAL;
    gstd_deployment_environment preparation_environment = {.database = db,
                                                           .settings = &settings,
                                                           .cache_fd = initialized.descriptors[0],
                                                           .resource_fd =
                                                               initialized.descriptors[4],
                                                           .volume_root = volume_owner};
    gstd_datastore preparation = {0};
    CHECK(!gstd_datastore_prepare_begin(&preparation_environment, &persistent_target,
                                        "preparation-configuration", &preparation));
    int prepared = 0;
    for (unsigned n = 0; n < 6000 && !prepared; n++) {
        prepared = gstd_datastore_prepare_poll(&preparation, -1);
        if (!prepared) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
    }
    CHECK(prepared == 1 && preparation.outcome == -1 &&
          preparation.detail == GSTD_ASSET_RECORD_CONSENT && preparation.graph.fresh_datastore &&
          preparation.volume.data_fd >= 3 && !preparation.workloads[0].launched);
    CHECK(!gstd_datastore_observers_close(&preparation));
    gstd_filesystem_reply_close(&datastore_configuration);
    gstd_deployment_target_clear(&persistent_target);
    json_decref(datastore_message);
    input = (gstd_filesystem_input){.action = GSTD_FS_IMAGE_VERIFY,
                                    .descriptors = {scope.descriptors[1], -1, -1}};
    struct stat directory;
    CHECK(!fstat(input.descriptors[0], &directory));
    data = openat(input.descriptors[0], "payload", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(data >= 3 && write(data, "before", 6) == 6 && !close(data));
    input.proof.staging_device = (uint64_t)directory.st_dev;
    input.proof.staging_inode = (uint64_t)directory.st_ino;
    CHECK(!gstd_asset_tree_digest(input.descriptors[0], 1048576, 0, input.proof.tree_digest,
                                  &input.proof.logical_bytes));
    CHECK(!run(&input, &again) && again.status == 0 && !again.descriptor_count);
    data = openat(input.descriptors[0], "payload", O_WRONLY | O_CLOEXEC);
    CHECK(data >= 3 && pwrite(data, "changed", 7, 0) == 7 && !close(data));
    CHECK(!run(&input, &again) && again.status == -1);
    input = (gstd_filesystem_input){.action = GSTD_FS_SCOPE,
                                    .store_id = expected.store_id,
                                    .operation_id = operation,
                                    .deployment_id = "one",
                                    .generation = 1,
                                    .resource_id = "paused-fixture",
                                    .resource_kind = "configuration",
                                    .descriptors = {initialized.descriptors[4], -1, -1},
                                    .test_pause = 1};
    gstd_filesystem_job job = {0};
    CHECK(!gstd_filesystem_launch(&input, 5000, &job));
    int status;
    CHECK(waitpid(job.worker, &status, WUNTRACED) == job.worker && WIFSTOPPED(status));
    int pipes[2];
    CHECK(!pipe2(pipes, O_CLOEXEC) && write(pipes[1], "x", 1) == 1);
    int ready = 0;
    for (unsigned i = 0; i < 2000 && !ready; i++) {
        ready = gstd_filesystem_poll(&job, pipes[0], &again);
        if (!ready) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
    }
    CHECK(ready == 1 && again.status == -2 && !again.descriptor_count && !job.worker);
    char marker;
    CHECK(read(pipes[0], &marker, 1) == 1 && marker == 'x');
    close(pipes[0]);
    close(pipes[1]);
    CHECK(faccessat(input.descriptors[0], "paused-fixture", F_OK, 0) != 0);
    CHECK(!reserve(db, "uncertain-fixture", "configuration", operation));
    input.resource_id = "uncertain-fixture";
    input.test_pause = 2;
    CHECK(!gstd_filesystem_launch(&input, 5000, &job));
    CHECK(waitpid(job.worker, &status, WUNTRACED) == job.worker && WIFSTOPPED(status));
    CHECK(!kill(job.worker, SIGSEGV) && !kill(job.worker, SIGCONT));
    ready = 0;
    for (unsigned i = 0; i < 2000 && !ready; i++) {
        ready = gstd_filesystem_poll(&job, -1, &again);
        if (!ready) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
    }
    CHECK(ready == 1 && again.status == -2 && !again.descriptor_count);
    CHECK(!faccessat(input.descriptors[0], "uncertain-fixture/owner.json", F_OK, 0));
    gstd_filesystem_reply_close(&scope);
    gstd_filesystem_reply_close(&workload);
    gstd_filesystem_reply_close(&volume);
    gstd_cache_owner saved_cache;
    CHECK(gstd_asset_record_cache_state(db, &saved_cache) == 0);
    uint64_t saved_volume_root = initialized.volume_root.inode;
    gstd_filesystem_reply_close(&initialized);
    input = (gstd_filesystem_input){.action = GSTD_FS_REOPEN_ROOTS,
                                    .store_id = saved_cache.store_id,
                                    .expected_cache = saved_cache,
                                    .descriptors = {root, -1, -1}};
    CHECK(!run(&input, &again) && !again.status && again.descriptor_count == 6 &&
          again.cache_owner.inode == saved_cache.inode &&
          again.volume_root.inode == saved_volume_root);
    gstd_filesystem_reply_close(&again);
    char native_name[80];
    snprintf(native_name, sizeof(native_name), "native-%s", saved_cache.store_id);
    CHECK(!renameat(root, native_name, root, "saved-native"));
    CHECK(!run(&input, &again) && again.status == -1 && !again.descriptor_count);
    CHECK(faccessat(root, native_name, F_OK, AT_SYMLINK_NOFOLLOW) < 0);
    CHECK(!renameat(root, "saved-native", root, native_name));
    CHECK(!mkdirat(root, "empty-recovery-state", 0700));
    int empty =
        openat(root, "empty-recovery-state", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(empty >= 3);
    input.descriptors[0] = empty;
    CHECK(!run(&input, &again) && again.status == -1 && !again.descriptor_count);
    CHECK(faccessat(empty, "assets", F_OK, 0) < 0 && faccessat(empty, "volumes", F_OK, 0) < 0);
    close(empty);

    sqlite3_close(db);
    json_decref(manifest);
    json_decref(components);
    json_decref(message);
    close(root);
    CHECK(!gstd_test_remove(path));
    return 0;
}
