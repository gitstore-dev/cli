#define _GNU_SOURCE 1
#include "deployment_records.h"
#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "helper_owner.h"
#include "migration.h"
#include "migration_datastore.h"
#include "support.h"
#include "volume_records.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d\n", __LINE__);                                                \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
#define SQL(x) CHECK(sqlite3_exec(db, x, NULL, NULL, NULL) == SQLITE_OK)
#ifdef __linux__
static int wait_migration_datastore(gstd_migration_datastore *actor) {
    for (unsigned i = 0; i < 15000; i++) {
        int ready = gstd_migration_datastore_poll(actor, -1);
        if (ready)
            return ready;
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    return -1;
}
static int migration_volume_case(sqlite3 *db, const gstd_volume_root *owner,
                                 const char *old_operation, const char *source, const char *volume,
                                 gstd_volume_handle *original) {
    int result = 0;
    gstd_deployment_target target = {0};
    json_t *message = NULL;
    CHECK(!gstd_deployment_target_read(db, source, &target));
    CHECK(!gstd_schedule_finish(db, old_operation));
    char scheduled[65], migration[65], reserved[65], producer[65];
    CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(scheduled, source));
    SQL("UPDATE operations SET state='succeeded',result='{}' WHERE method='deployment.start';"
        "UPDATE deployments SET desired_state='stopped',writer_ownership='released',generation=3;"
        "UPDATE resource_ownership SET ownership_state='released'");
    CHECK(!gstd_schedule_finish(db, source));
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(
              db,
              "INSERT INTO resource_ownership SELECT 'cfg-'||?,"
              "'configuration','one',?,'retained',json_object('schemaVersion',1,'kind','"
              "configuration',"
              "'storeId',store_id,'operationId',?,'deploymentId','one','generation',2,"
              "'profileDigest',json_extract(?,'$.profileDigest'),'executionManifest',json(?)),"
              "'released','2026-11-05T00:00:00Z' FROM query_meta",
              -1, &s, NULL) == SQLITE_OK);
    char *bytes = gstd_json_dump(target.manifest);
    CHECK(bytes);
    for (int i = 1; i <= 3; i++)
        sqlite3_bind_text(s, i, source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 4, bytes, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 5, bytes, -1, SQLITE_TRANSIENT);
    int inserted = sqlite3_step(s);
    sqlite3_finalize(s);
    free(bytes);
    CHECK(inserted == SQLITE_DONE);
    message = json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:b}}", "version", "major", 1, "minor",
                        0, "requestId", "migration", "method", "migration.start", "idempotencyKey",
                        "volume-migration", "params", "deploymentId", "one", "targetVersion",
                        "gitstore-06152d-schema-012", "backupAcknowledged", 1);
    gstd_request request;
    CHECK(message && !gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, NULL, NULL, migration) == GSTD_ACCEPT_NEW);
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_scylla_migration_volume_reserve(db, migration, reserved) ==
          GSTD_ACCEPT_MIGRATION_PRECONDITION);
    SQL("ROLLBACK");
    CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(scheduled, migration));
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_scylla_volume_reserve(db, migration, reserved) == GSTD_ACCEPT_NOT_FOUND);
    CHECK(gstd_scylla_migration_volume_reserve(db, migration, reserved) == GSTD_ACCEPT_NEW &&
          !strcmp(reserved, volume));
    CHECK(gstd_scylla_migration_volume_reserve(db, migration, reserved) == GSTD_ACCEPT_REPLAY);
    gstd_volume_identity expected;
    CHECK(gstd_scylla_volume_record_begin(db, owner, volume, migration, producer, &expected) == 1 &&
          !strcmp(producer, old_operation));
    char observed_volume[65], observed_producer[65];
    gstd_volume_identity observed_identity;
    CHECK(!gstd_scylla_volume_record_recovery_read(db, owner, migration, observed_volume,
                                                   observed_producer, &observed_identity));
    CHECK(!strcmp(observed_volume, volume) && !strcmp(observed_producer, old_operation) &&
          observed_identity.data_inode == expected.data_inode);
    CHECK(gstd_volume_record_begin(db, owner, volume, migration, producer, &expected) < 0);
    CHECK(gstd_volume_record_recovery_read(db, owner, migration, observed_volume, observed_producer,
                                           &observed_identity) < 0);
    /* A metadata reservation cannot overrule the original kernel writer. */
    CHECK(gstd_scylla_volume_record_begin(db, owner, volume, migration, producer, &expected) == 1);
    SQL("COMMIT");
    int marker = openat(original->data_fd, "migration-preserved",
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    const char retained[] = "retained migration payload";
    CHECK(marker >= 3 && write(marker, retained, sizeof(retained)) == sizeof(retained) &&
          !fsync(marker));
    close(marker);
    gstd_migration_datastore actor = {0};
    CHECK(!gstd_migration_datastore_begin(db, owner, migration, &actor));
    CHECK(wait_migration_datastore(&actor) == 1 && actor.outcome == -1 &&
          actor.detail == GSTD_VOLUME_HELD);
    CHECK(!gstd_migration_datastore_close(&actor));
    gstd_volume_handle_close(original);
    CHECK(!gstd_migration_datastore_begin(db, owner, migration, &actor));
    CHECK(wait_migration_datastore(&actor) == 1 && !actor.outcome);
    CHECK(actor.volume.identity.data_inode == expected.data_inode &&
          actor.volume.identity.writer_inode == expected.writer_inode &&
          !strcmp(actor.producer_id, old_operation));
    marker = openat(actor.volume.data_fd, "migration-preserved", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    char payload[sizeof(retained)];
    CHECK(marker >= 3 && read(marker, payload, sizeof(payload)) == sizeof(payload) &&
          !memcmp(payload, retained, sizeof(payload)));
    close(marker);
    CHECK(!gstd_migration_datastore_close(&actor));
    /* A separately acknowledged retry may reacquire a prior migration's
     * released volume, but never an uncertain owner or unresolved helper. */
    SQL("BEGIN IMMEDIATE;UPDATE resource_ownership SET ownership_state='released',"
        "lifecycle_stage='retained' WHERE resource_type='datastore-volume'");
    CHECK(!gstd_migration_finish(db, migration, 0, 1));
    SQL("COMMIT");
    CHECK(!gstd_schedule_finish(db, migration));
    CHECK(!json_object_set_new(message, "idempotencyKey", json_string("volume-migration-retry")));
    CHECK(!gstd_request_parse(message, &request));
    char retry[65];
    CHECK(gstd_accept(db, &request, NULL, NULL, retry) == GSTD_ACCEPT_NEW);
    CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(scheduled, retry));
    SQL("BEGIN IMMEDIATE;SAVEPOINT uncertain_previous_volume;"
        "UPDATE resource_ownership SET ownership_state='uncertain' WHERE "
        "resource_type='datastore-volume'");
    CHECK(gstd_scylla_migration_volume_reserve(db, retry, reserved) ==
          GSTD_ACCEPT_MIGRATION_PRECONDITION);
    SQL("ROLLBACK TO uncertain_previous_volume;RELEASE uncertain_previous_volume");
    CHECK(gstd_scylla_migration_volume_reserve(db, retry, reserved) == GSTD_ACCEPT_NEW &&
          !strcmp(volume, reserved));
    CHECK(gstd_scylla_volume_record_begin(db, owner, volume, retry, producer, &expected) == 1 &&
          !strcmp(producer, old_operation));
    SQL("COMMIT");

done:
    gstd_deployment_target_clear(&target);
    json_decref(message);
    return result;
}
static int graph_case(sqlite3 *db, const char *operation, const char *volume, int fresh) {
    int result = 0, scope = -1, data = -1, helper_scope = -1, helper_execution = -1,
        helper_bundle = -1, helper_root = -1;
    char *root = gstd_test_dir();
    gstd_deployment_target target = {0};
    CHECK(root && !gstd_deployment_target_read(db, operation, &target));
    CHECK(!strcmp(json_string_value(json_object_get(target.manifest, "datastore")), "scylladb"));
    scope = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(scope >= 3 && !mkdirat(scope, "data", 0700));
    data = openat(scope, "data", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat info;
    CHECK(data >= 3 && !fstat(scope, &info));
    gstd_filesystem_reply receipt = {.descriptor_count = 2,
                                     .descriptors = {scope, data},
                                     .scope_device = (uint64_t)info.st_dev,
                                     .scope_inode = (uint64_t)info.st_ino};
    gstd_bootstrap_graph graph = {0};
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_deployment_resource_reserve(db, &target, "graph-fixture", "configuration"));
    CHECK(!gstd_deployment_scope_record(db, &target, "graph-fixture", &receipt));
    CHECK(gstd_deployment_persistent_graph_begin(db, &target, "graph-fixture", "foreign", &graph) <
          0);
    CHECK(!gstd_deployment_persistent_graph_begin(db, &target, "graph-fixture", volume, &graph));
    CHECK(graph.persistent && graph.fresh_datastore == (unsigned)fresh);
    CHECK(!gstd_deployment_graph_matches(db, &target, "graph-fixture", &graph));
    SQL("SAVEPOINT policy_binding");
    SQL("UPDATE resource_ownership SET backend_identity=json_set(backend_identity,"
        "'$.executionManifest.migrationTarget','foreign-target') WHERE "
        "resource_id='graph-fixture'");
    CHECK(gstd_deployment_graph_matches(db, &target, "graph-fixture", &graph) < 0);
    SQL("ROLLBACK TO policy_binding");
    SQL("RELEASE policy_binding");
    CHECK(!gstd_deployment_graph_matches(db, &target, "graph-fixture", &graph));
    gstd_bootstrap_graph stale = graph;
    stale.stage = GSTD_BOOT_WAIT_DATASTORE;
    CHECK(gstd_deployment_graph_matches(db, &target, "graph-fixture", &stale) < 0);
    CHECK(gstd_deployment_persistent_graph_begin(db, &target, "graph-fixture", volume, &graph) < 0);
    CHECK(!gstd_deployment_graph_step(db, &target, "graph-fixture", &graph, GSTD_BOOT_PREPARE, 0,
                                      NULL));
    CHECK(!gstd_deployment_graph_step(db, &target, "graph-fixture", &graph,
                                      GSTD_BOOT_LAUNCH_DATASTORE, 0, NULL));
    /* Controlled metadata model only. It grants no native execution or
     * datastore lease; helper-created/finished writes require kernel handles. */
    char source[65], helper_id[65];
    CHECK(snprintf(source, sizeof(source), "run-scylladb-%s", operation) > 0);
    CHECK(snprintf(helper_id, sizeof(helper_id), "run-cql-%s", operation) > 0);
    CHECK(!gstd_deployment_resource_reserve(db, &target, source, "workload"));
    CHECK(!gstd_deployment_resource_reserve(db, &target, helper_id, "workload"));
    CHECK(!mkdirat(scope, "helper", 0700));
    helper_scope = openat(scope, "helper", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(helper_scope >= 3 && !mkdirat(helper_scope, "execution", 0700) &&
          !mkdirat(helper_scope, "bundle", 0700));
    helper_execution = openat(helper_scope, "execution", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    helper_bundle = openat(helper_scope, "bundle", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(helper_execution >= 3 && helper_bundle >= 3 &&
          !mkdirat(helper_execution, "rootfs", 0700));
    helper_root = openat(helper_execution, "rootfs", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(helper_root >= 3 && !fstat(helper_scope, &info));
    gstd_filesystem_reply helper_receipt = {
        .descriptor_count = 3,
        .descriptors = {helper_scope, helper_execution, helper_bundle},
        .scope_device = (uint64_t)info.st_dev,
        .scope_inode = (uint64_t)info.st_ino};
    CHECK(!gstd_deployment_scope_record(db, &target, helper_id, &helper_receipt));
    CHECK(!gstd_deployment_execution_record(db, &target, helper_id, &helper_receipt, helper_root));
    sqlite3_stmt *source_row = NULL;
    CHECK(sqlite3_prepare_v2(db,
                             "UPDATE resource_ownership SET lifecycle_stage='running',"
                             "backend_identity=json_object('executionManifest',json(?),"
                             "'native',json_object('kind','libcrun',"
                             "'id',resource_id,'component','scylladb','generation',?,'pid',42,"
                             "'startTime',1,'bootId','01234567-89ab-cdef-0123-456789abcdef')) "
                             "WHERE resource_id=?",
                             -1, &source_row, NULL) == SQLITE_OK);
    char *manifest_bytes = gstd_json_dump(target.manifest);
    CHECK(manifest_bytes);
    sqlite3_bind_text(source_row, 1, manifest_bytes, -1, SQLITE_TRANSIENT);
    free(manifest_bytes);
    sqlite3_bind_int64(source_row, 2, (sqlite3_int64)target.generation);
    sqlite3_bind_text(source_row, 3, source, -1, SQLITE_TRANSIENT);
    int stored_source = sqlite3_step(source_row);
    sqlite3_finalize(source_row);
    CHECK(stored_source == SQLITE_DONE && sqlite3_changes(db) == 1);
    CHECK(gstd_deployment_helper_reserve(db, &target, "graph-fixture", "foreign",
                                         GSTD_EXEC_SCYLLA_READY) < 0);
    int admitted = gstd_deployment_helper_reserve(db, &target, "graph-fixture", helper_id,
                                                  GSTD_EXEC_SCYLLA_READY);
    CHECK(fresh ? admitted < 0 : admitted == 0);
    CHECK(gstd_deployment_helper_reserve(db, &target, "graph-fixture", helper_id,
                                         GSTD_EXEC_SCYLLA_READY) < 0);
    gstd_helper invented = {.death_fd = -1,
                            .deleted = 1,
                            .outcome = 0,
                            .population = GSTD_NATIVE_POPULATION_INITIAL,
                            .exit = {.exited = 1}};
    CHECK(gstd_deployment_helper_created(db, &target, helper_id, &invented) < 0);
    CHECK(gstd_deployment_helper_finished(db, &target, helper_id, &invented) < 0);
    CHECK(gstd_deployment_graph_step(db, &target, "graph-fixture", &graph, GSTD_BOOT_WAIT_DATASTORE,
                                     0, NULL) < 0);
    CHECK(graph.stage == GSTD_BOOT_WAIT_DATASTORE);
    CHECK(!gstd_deployment_graph_matches(db, &target, "graph-fixture", &graph));
    SQL("ROLLBACK");
    /* A forged pre-START notification must never cross the owner commit gate.
     * This is deliberately missing all kernel handles; no native worker runs. */
    gstd_helper_owner rejected = {
        .database = db,
        .target = &target,
        .stage = 1,
        .helper = {.stage = 2, /* private helper awaiting identity commit */
                   .may_exist = 1,
                   .death_fd = -1,
                   .population = GSTD_NATIVE_POPULATION_INITIAL,
                   .context = {.backend_id = helper_id}}};
    int changes = sqlite3_total_changes(db);
    CHECK(gstd_helper_owner_poll(&rejected, -1) == 1 && rejected.outcome == -2 &&
          rejected.helper.outcome == -2 && rejected.helper.may_exist && !rejected.helper.deleted &&
          !rejected.helper.worker.worker && sqlite3_get_autocommit(db) &&
          sqlite3_total_changes(db) == changes);
    CHECK(gstd_helper_owner_poll(&rejected, -1) == 1 && rejected.outcome == -2);
    CHECK(!gstd_helper_owner_close(&rejected) && sqlite3_total_changes(db) == changes);
done:
    if (!sqlite3_get_autocommit(db))
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    gstd_deployment_target_clear(&target);
    if (helper_root >= 0)
        close(helper_root);
    if (helper_bundle >= 0)
        close(helper_bundle);
    if (helper_execution >= 0)
        close(helper_execution);
    if (helper_scope >= 0)
        close(helper_scope);
    if (data >= 0)
        close(data);
    if (scope >= 0)
        close(scope);
    if (root && gstd_test_remove(root))
        result = 1;
    return result;
}
#endif
static int scenario(int scylla) {
    int result = 0, root_fd = -1;
    char *root = gstd_test_dir(), path[1024], operation[65], producer[65];
    sqlite3 *db = NULL;
    json_t *message = NULL, *components = NULL, *manifest = NULL;
    gstd_volume_root owner = {.lock_fd = -1};
    gstd_volume_handle handle = {.data_fd = -1, .writer_fd = -1},
                       paired = {.data_fd = -1, .writer_fd = -1};
    gstd_volume_identity expected;
    const char *volume = scylla ? "datastore" : "repositories";
    gstd_json_initialize();
    CHECK(root);
    snprintf(path, sizeof(path), "%s/state.db", root);
    CHECK(!gstd_state_open(path, &db));
    CHECK((scylla ? gstd_scylla_volume_record_begin : gstd_volume_record_begin)(
              db, &owner, volume, "operation", producer, &expected) == -1);
    components = json_pack("[s,s]", "api", "git");
#if defined(__aarch64__) || defined(__arm64__)
    const char *architecture = "arm64", *platform = "linux-arm64";
#else
    const char *architecture = "amd64", *platform = "linux-amd64";
#endif
    manifest = gstd_asset_manifest(components, architecture);
    CHECK(manifest);
    sqlite3_stmt *profile = NULL;
    CHECK(sqlite3_prepare_v2(db, "INSERT INTO profiles VALUES('core','1',?,?,?)", -1, &profile,
                             NULL) == SQLITE_OK);
    sqlite3_bind_text(profile, 1, json_string_value(json_object_get(manifest, "serverRevision")),
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(profile, 2, json_string_value(json_object_get(manifest, "profileDigest")), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(profile, 3, platform, -1, SQLITE_TRANSIENT);
    int inserted = sqlite3_step(profile);
    sqlite3_finalize(profile);
    CHECK(inserted == SQLITE_DONE);
    const char request_text[] =
        "{\"version\":{\"major\":1,\"minor\":0},\"requestId\":\"r\",\"method\":\"deployment."
        "start\",\"idempotencyKey\":\"volume-start\",\"params\":{\"deploymentId\":\"one\","
        "\"profileId\":\"core\",\"profileVersion\":\"1\"}}";
    message = gstd_json_parse((const unsigned char *)request_text, sizeof(request_text) - 1);
    gstd_request request;
    if (scylla && message)
        CHECK(!json_object_set_new(json_object_get(message, "params"), "datastore",
                                   json_string("scylladb")));
    CHECK(message && components && !gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, components, scylla ? NULL : volume, operation) ==
          GSTD_ACCEPT_NEW);
#ifdef __linux__
    sqlite3_stmt *query = NULL;
    char canonical[65];
    SQL("BEGIN IMMEDIATE");
    if (scylla) {
        CHECK(gstd_scylla_volume_reserve(db, operation, canonical) == GSTD_ACCEPT_NEW);
        volume = canonical;
        char replayed[65];
        CHECK(gstd_scylla_volume_reserve(db, operation, replayed) == GSTD_ACCEPT_REPLAY &&
              !strcmp(volume, replayed));
    } else
        CHECK(gstd_scylla_volume_reserve(db, operation, canonical) == GSTD_ACCEPT_NOT_FOUND);
    SQL("COMMIT");
    CHECK(sqlite3_prepare_v2(db, "SELECT store_id FROM query_meta", -1, &query, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(query) == SQLITE_ROW);
    char store[33];
    snprintf(store, sizeof(store), "%s", sqlite3_column_text(query, 0));
    sqlite3_finalize(query);
    snprintf(path, sizeof(path), "%s/volumes", root);
    CHECK(!mkdir(path, 0700));
    root_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(root_fd >= 3 && gstd_volume_root_lock(root_fd, store, &owner) == GSTD_VOLUME_OK);
    SQL("BEGIN IMMEDIATE");
    CHECK((scylla ? gstd_volume_record_begin : gstd_scylla_volume_record_begin)(
              db, &owner, volume, operation, producer, &expected) == -1);
    CHECK((scylla ? gstd_scylla_volume_record_begin : gstd_volume_record_begin)(
              db, &owner, volume, "foreign", producer, &expected) == -1);
    SQL("ROLLBACK");
    SQL("BEGIN IMMEDIATE");
    CHECK((scylla ? gstd_scylla_volume_record_begin : gstd_volume_record_begin)(
              db, &owner, volume, operation, producer, &expected) == 0);
    CHECK(!strcmp(producer, operation) && !expected.data_inode);
    SQL("COMMIT");
    if (scylla)
        CHECK(!graph_case(db, operation, volume, 1));
    CHECK((scylla ? gstd_scylla_volume_acquire : gstd_volume_acquire)(
              root_fd, &owner, volume, producer, 1, NULL, &handle) == GSTD_VOLUME_OK);
    CHECK((scylla ? gstd_scylla_volume_record_ready
                  : gstd_volume_record_ready)(db, &owner, volume, operation, &handle) == -1);
    if (scylla) {
        SQL("INSERT INTO resource_ownership SELECT "
            "'paired','repository-volume',deployment_id,operation_id,'reserved',NULL,'held',"
            "observed_at FROM resource_ownership WHERE resource_type='datastore-volume'");
        SQL("BEGIN IMMEDIATE");
        CHECK(gstd_volume_record_begin(db, &owner, "paired", operation, producer, &expected) == 0);
        SQL("COMMIT");
        CHECK(gstd_volume_acquire(root_fd, &owner, "paired", producer, 1, NULL, &paired) ==
              GSTD_VOLUME_OK);
        SQL("BEGIN IMMEDIATE");
        CHECK(!gstd_volume_record_ready(db, &owner, "paired", operation, &paired));
        CHECK(gstd_scylla_volume_record_ready(db, &owner, volume, operation, &paired) == -1);
        SQL("COMMIT");
        gstd_volume_handle_close(&paired);
    }
    SQL("BEGIN IMMEDIATE");
    CHECK(!(scylla ? gstd_scylla_volume_record_ready
                   : gstd_volume_record_ready)(db, &owner, volume, operation, &handle));
    CHECK(!(scylla ? gstd_scylla_volume_record_ready
                   : gstd_volume_record_ready)(db, &owner, volume, operation, &handle));
    SQL("COMMIT");
    if (scylla)
        CHECK(!graph_case(db, operation, volume, 0));
    char saved_volume[65], saved_producer[65];
    gstd_volume_identity saved_identity;
    CHECK(!(scylla ? gstd_scylla_volume_record_recovery_read : gstd_volume_record_recovery_read)(
        db, &owner, operation, saved_volume, saved_producer, &saved_identity));
    CHECK(!strcmp(saved_volume, volume) && !strcmp(saved_producer, operation) &&
          saved_identity.data_inode == handle.identity.data_inode &&
          saved_identity.writer_inode == handle.identity.writer_inode);
    SQL("BEGIN IMMEDIATE;UPDATE resource_ownership SET ownership_state='released'");
    CHECK((scylla ? gstd_scylla_volume_record_recovery_read : gstd_volume_record_recovery_read)(
              db, &owner, operation, saved_volume, saved_producer, &saved_identity) == -1);
    SQL("ROLLBACK");

    SQL("BEGIN IMMEDIATE");
    CHECK((scylla ? gstd_scylla_volume_record_begin : gstd_volume_record_begin)(
              db, &owner, volume, operation, producer, &expected) == 1);
    CHECK(expected.data_inode == handle.identity.data_inode &&
          expected.writer_inode == handle.identity.writer_inode);
    SQL("COMMIT");
    CHECK(sqlite3_exec(db,
                       scylla ? "UPDATE datastore_volumes SET allocated=0"
                              : "UPDATE repository_volumes SET allocated=0",
                       NULL, NULL, NULL) != SQLITE_OK);
    CHECK(sqlite3_exec(db,
                       scylla ? "DELETE FROM datastore_volumes" : "DELETE FROM repository_volumes",
                       NULL, NULL, NULL) != SQLITE_OK);
    gstd_volume_root changed = owner;
    changed.inode++;
    SQL("BEGIN IMMEDIATE");
    CHECK((scylla ? gstd_scylla_volume_record_begin : gstd_volume_record_begin)(
              db, &changed, volume, operation, producer, &expected) == -1);
    SQL("ROLLBACK");
    CHECK(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    snprintf(path, sizeof(path), "%s/state.db", root);
    CHECK(!gstd_state_open(path, &db));
    SQL("BEGIN IMMEDIATE");
    CHECK((scylla ? gstd_scylla_volume_record_begin : gstd_volume_record_begin)(
              db, &owner, volume, operation, producer, &expected) == 1);
    SQL("COMMIT");
    if (scylla) {
        char taken[65];
        CHECK(gstd_schedule_take(db, 0, taken) == 1 && !strcmp(taken, operation));
        SQL("UPDATE operations SET state='succeeded',result='{}' WHERE method='deployment.start'");
        json_t *next_message = json_deep_copy(message);
        CHECK(next_message && !json_object_set_new(next_message, "idempotencyKey",
                                                   json_string("datastore-replacement")));
        gstd_request next_request;
        char next_operation[65], replayed[65];
        CHECK(!gstd_request_parse(next_message, &next_request));
        gstd_accept_status accepted =
            gstd_accept(db, &next_request, components, NULL, next_operation);
        CHECK(accepted == GSTD_ACCEPT_OWNERSHIP_UNCERTAIN);
        /* Admission now rejects every unresolved owner even when the caller
         * has no repository-volume selection. Settle metadata before testing
         * the separate volume reservation/kernel writer guards below. */
        SQL("UPDATE resource_ownership SET ownership_state='released';"
            "UPDATE deployments SET writer_ownership='released'");
        accepted = gstd_accept(db, &next_request, components, NULL, next_operation);
        json_decref(next_message);
        CHECK(accepted == GSTD_ACCEPT_NEW);
        SQL("BEGIN IMMEDIATE;UPDATE resource_ownership SET ownership_state='held' WHERE "
            "resource_type='datastore-volume'");
        CHECK(gstd_scylla_volume_reserve(db, next_operation, replayed) ==
              GSTD_ACCEPT_OWNERSHIP_UNCERTAIN);
        SQL("UPDATE resource_ownership SET ownership_state='released' WHERE "
            "resource_type='datastore-volume'");
        CHECK(gstd_scylla_volume_reserve(db, next_operation, replayed) == GSTD_ACCEPT_NEW &&
              !strcmp(volume, replayed));
        CHECK(gstd_scylla_volume_record_begin(db, &owner, volume, next_operation, producer,
                                              &expected) == 1 &&
              !strcmp(producer, operation));
        SQL("COMMIT");
        CHECK(!graph_case(db, next_operation, volume, 0));
        gstd_volume_handle replacement = {.data_fd = -1, .writer_fd = -1};
        CHECK(gstd_scylla_volume_acquire(root_fd, &owner, volume, producer, 0, &expected,
                                         &replacement) == GSTD_VOLUME_HELD);
        gstd_volume_handle_close(&handle);
        CHECK(gstd_scylla_volume_acquire(root_fd, &owner, volume, producer, 0, &expected,
                                         &handle) == GSTD_VOLUME_OK);
        CHECK(handle.identity.data_inode == expected.data_inode);
        CHECK(!migration_volume_case(db, &owner, operation, next_operation, volume, &handle));
    }
#endif
done:
    gstd_volume_handle_close(&paired);
    gstd_volume_handle_close(&handle);
    gstd_volume_root_close(&owner);
    if (root_fd >= 0)
        close(root_fd);
    json_decref(message);
    json_decref(components);
    json_decref(manifest);
    if (db)
        sqlite3_close(db);
    if (root && gstd_test_remove(root))
        result = 1;
    return result;
}

int main(void) { return scenario(0) || scenario(1); }
