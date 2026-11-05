#define _DARWIN_C_SOURCE 1
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/asset_cache.h"
#include "gitstored/asset_records.h"
#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "migration.h"
#include "migration_execution.h"
#include "migration_history.h"
#include "migration_recovery_records.h"
#include "support.h"
#include "volume_records.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "migration line %d: %s\n", __LINE__, #x);                              \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int resource_name(char output[65], const char *prefix, const char *operation) {
    int length = snprintf(output, 65, "%s%s", prefix, operation);
    return length > 0 && length < 65 ? 0 : -1;
}
static json_t *message(const char *key, const char *target, int backup) {
    return json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:b}}", "version", "major", 1, "minor", 0,
                     "requestId", "request", "method", "migration.start", "idempotencyKey", key,
                     "params", "deploymentId", "persistent", "targetVersion", target,
                     "backupAcknowledged", backup);
}
static gstd_accept_status accept(sqlite3 *db, json_t *value, char id[65]) {
    gstd_request request;
    return gstd_request_parse(value, &request) ? GSTD_ACCEPT_INVALID
                                               : gstd_accept(db, &request, NULL, NULL, id);
}
static int scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        value = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return value;
}
static int cql_decode_case(void) {
    int result = 1;
    const char *one = "\n [json]\n------\n {\"x\":1}\n\n(1 rows)\n";
    json_t *decoded = gstd_migration_cql_decode(one, strlen(one), 1);
    CHECK(decoded && json_array_size(decoded) == 1 &&
          json_array_size(json_array_get(decoded, 0)) == 1);
    json_decref(decoded);
    decoded = NULL;
    CHECK(!gstd_migration_cql_decode(one, strlen(one), 2));
    CHECK(!gstd_migration_cql_decode(one, 65537, 1));
    CHECK(!gstd_migration_cql_decode(one, strlen(one), 0));
    const char *bad[] = {"[json]\n---\n{\"x\":1}\n",
                         "[json]\n---\n{\"x\":1}\n(0 rows)",
                         "[json]\n---\n{\"x\":1,\"x\":2}\n(1 rows)",
                         "[json]\nxxx\n(0 rows)",
                         "[json]\n---\n(0 rows)\nextra",
                         "Schema migrations complete.",
                         "[json]\n---\n[1]\n(1 rows)"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++)
        CHECK(!gstd_migration_cql_decode(bad[i], strlen(bad[i]), 1));
    char pair[256];
    int n = snprintf(pair, sizeof(pair), "%s\n[json]\n------\n\n(0 rows)\n", one);
    CHECK(n > 0 && n < (int)sizeof(pair));
    decoded = gstd_migration_cql_decode(pair, (size_t)n, 2);
    CHECK(decoded && json_array_size(decoded) == 2 && !json_array_size(json_array_get(decoded, 1)));
    json_decref(decoded);
    decoded = NULL;
    char many[2048];
    size_t length = 0;
    length += (size_t)snprintf(many + length, sizeof(many) - length, "[json]\n------\n");
    for (unsigned i = 0; i < 14; i++)
        length += (size_t)snprintf(many + length, sizeof(many) - length, "{\"x\":1}\n");
    length += (size_t)snprintf(many + length, sizeof(many) - length, "(14 rows)\n");
    CHECK(!gstd_migration_cql_decode(many, length, 1));
#ifdef __linux__
    int output = gstd_migration_output_create();
    CHECK(output >= 3 && write(output, one, strlen(one)) == (ssize_t)strlen(one));
    decoded = gstd_migration_output_rows(output, 1);
    CHECK(decoded && json_array_size(decoded) == 1);
    errno = 0;
    CHECK(pwrite(output, "x", 1, 0) < 0 && errno == EPERM);
    close(output);
    json_decref(decoded);
    decoded = NULL;
    output = gstd_migration_output_create();
    CHECK(output >= 3);
    CHECK(ftruncate(output, 65537) < 0 && errno == EPERM);
    CHECK(lseek(output, 65535, SEEK_SET) == 65535 && write(output, "x", 1) == 1);
    errno = 0;
    CHECK(write(output, "x", 1) < 0 && errno == EPERM);
    CHECK(!gstd_migration_output_rows(output, 1));
    close(output);
#endif
    result = 0;
done:
    json_decref(decoded);
    return result;
}
static int schema_inspection_case(void) {
    int result = 1, ledger = 7;
    json_t *sets =
        json_pack("[[{s:s}],[{s:s}]]", "keyspace_name", "gitstore", "table_name", "gocqlx_migrate");
    CHECK(sets && !gstd_migration_schema_inspection(sets, &ledger) && ledger == 1);
    CHECK(!gstd_migration_schema_inspection(sets, &ledger) && ledger == 1);
    CHECK(!json_array_clear(json_array_get(sets, 1)));
    CHECK(!gstd_migration_schema_inspection(sets, &ledger) && ledger == 0);
    CHECK(!json_object_set_new(json_array_get(json_array_get(sets, 0), 0), "extra", json_true()));
    CHECK(gstd_migration_schema_inspection(sets, &ledger) < 0 && !ledger);
    CHECK(!json_object_del(json_array_get(json_array_get(sets, 0), 0), "extra"));
    CHECK(!json_array_clear(json_array_get(sets, 0)));
    CHECK(gstd_migration_schema_inspection(sets, &ledger) < 0 && !ledger);
    CHECK(gstd_migration_schema_inspection(NULL, &ledger) < 0 && !ledger);
    CHECK(gstd_migration_schema_inspection(sets, NULL) < 0);
    result = 0;
done:
    json_decref(sets);
    return result;
}
static int history_case(void) {
    int result = 1;
    const char *version = "gitstore-06152d-schema-012";
    json_t *target = gstd_migration_target(version), *rows = json_array(), *changed = NULL;
    CHECK(target && rows);
    json_t *files = json_object_get(json_object_get(target, "schema"), "files");
    CHECK(!gstd_migration_history_validate(version, rows, 0));
    CHECK(gstd_migration_history_validate(version, rows, 1) < 0);
    for (size_t i = 0; i < json_array_size(files); i++) {
        json_t *file = json_array_get(files, i);
        json_t *row = json_pack("{s:O,s:O,s:O,s:s}", "name", json_object_get(file, "name"),
                                "checksum", json_object_get(file, "ledgerMd5"), "done",
                                json_object_get(file, "statementCount"), "end_time",
                                "2026-11-04 20:40:02.822Z");
        CHECK(row && !json_array_append_new(rows, row));
    }
    CHECK(!gstd_migration_history_validate(version, rows, 1));
    CHECK(gstd_migration_history_validate("unknown", rows, 0) < 0);
    CHECK(gstd_migration_history_validate(version, rows, 2) < 0);
    changed = json_deep_copy(rows);
    CHECK(changed && !json_array_remove(changed, 0));
    CHECK(gstd_migration_history_validate(version, changed, 0) < 0); /* gap */
    json_decref(changed);
    changed = json_deep_copy(rows);
    CHECK(changed && !json_array_set(changed, 0, json_array_get(changed, 1)));
    CHECK(gstd_migration_history_validate(version, changed, 0) < 0); /* duplicate */
    json_decref(changed);
    changed = json_deep_copy(rows);
    json_t *row = json_array_get(changed, json_array_size(changed) - 1);
    json_int_t statements = json_integer_value(json_object_get(row, "done"));
    CHECK(!json_object_set_new(row, "done", json_integer(statements - 1)));
    CHECK(!json_object_set_new(row, "end_time", json_null()));
    CHECK(!gstd_migration_history_validate(version, changed, 0));
    CHECK(gstd_migration_history_validate(version, changed, 1) < 0);
    CHECK(!json_object_set_new(json_array_get(changed, 0), "end_time", json_null()));
    CHECK(gstd_migration_history_validate(version, changed, 0) < 0); /* older incomplete entry */
    json_decref(changed);
    changed = json_deep_copy(rows);
    row = json_array_get(changed, 0);
    const char *invalid[] = {"0001-01-01 00:00:00.000Z", "2025-02-29 20:40:02.822Z",
                             "2026-04-31 20:40:02.822Z", "2026-11-04 24:40:02.822Z",
                             "2026-11-04 20:40:02.822Zextra"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        CHECK(!json_object_set_new(row, "end_time", json_string(invalid[i])));
        CHECK(gstd_migration_history_validate(version, changed, 1) < 0);
    }
    CHECK(!json_object_set_new(row, "end_time", json_string("2024-02-29 20:40:02.822Z")));
    CHECK(!gstd_migration_history_validate(version, changed, 1));
    CHECK(!json_object_set_new(row, "checksum", json_string("wrong")));
    CHECK(gstd_migration_history_validate(version, changed, 0) < 0);
    json_decref(changed);
    changed = json_deep_copy(rows);
    row = json_array_get(changed, 0);
    CHECK(!json_object_set_new(row, "done", json_integer(-1)));
    CHECK(gstd_migration_history_validate(version, changed, 0) < 0);
    CHECK(!json_object_set_new(row, "done", json_integer(1000000)));
    CHECK(gstd_migration_history_validate(version, changed, 0) < 0);
    CHECK(!json_array_set(changed, 0, json_array_get(rows, 0)));
    CHECK(!json_object_set_new(json_array_get(changed, 0), "extra", json_true()));
    CHECK(gstd_migration_history_validate(version, changed, 1) < 0);
    result = 0;
done:
    json_decref(changed);
    json_decref(rows);
    json_decref(target);
    return result;
}
static int fence_identity(sqlite3 *db, const char *id, json_t *value, const char *stage) {
    char *bytes = gstd_json_dump(value);
    sqlite3_stmt *s = NULL;
    int ok = bytes && sqlite3_prepare_v2(db,
                                         "UPDATE resource_ownership SET "
                                         "backend_identity=?,lifecycle_stage=? WHERE resource_id=?",
                                         -1, &s, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(s, 1, bytes, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, stage, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, id, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    }
    sqlite3_finalize(s);
    free(bytes);
    return ok ? 0 : -1;
}
static int migration_attachment_case(sqlite3 *db, const gstd_deployment_target *target) {
    int result = 1;
    char network[65], source[65], forbidden[65], read_helper[65];
    CHECK(!resource_name(network, "net-", target->operation_id));
    CHECK(!resource_name(source, "run-scylladb-", target->operation_id));
    CHECK(!resource_name(forbidden, "run-api-", target->operation_id));
    CHECK(!resource_name(read_helper, "run-cql-", target->operation_id));
    CHECK(sqlite3_exec(db, "SAVEPOINT migration_attachment", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!gstd_deployment_resource_reserve(db, target, network, "network"));
    CHECK(!gstd_deployment_resource_reserve(db, target, source, "workload"));
    CHECK(!gstd_deployment_resource_reserve(db, target, forbidden, "workload"));
    CHECK(!gstd_deployment_resource_reserve(db, target, read_helper, "workload"));
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(db,
                             "UPDATE resource_ownership SET backend_identity='{}',"
                             "lifecycle_stage='running' WHERE resource_id=?",
                             -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, network, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    int attachment_prepared =
        sqlite3_prepare_v2(db,
                           "INSERT INTO network_attachments "
                           "VALUES(?,?,?,'00000000-0000-0000-0000-000000000001','1','2','3','4')",
                           -1, &s, NULL);
    if (attachment_prepared != SQLITE_OK)
        fprintf(stderr, "attachment prepare: %s\n", sqlite3_errmsg(db));
    CHECK(attachment_prepared == SQLITE_OK);
    sqlite3_bind_text(s, 1, forbidden, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, network, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, target->operation_id, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(s) != SQLITE_DONE);
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, source, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_reset(s);
    gstd_migration_observation observations = {1, 1, 1, 1};
    CHECK(!gstd_migration_prepare(db, target->operation_id, &observations));
    sqlite3_bind_text(s, 1, read_helper, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(s) != SQLITE_DONE);
    sqlite3_finalize(s);
    CHECK(sqlite3_exec(db, "ROLLBACK TO migration_attachment;RELEASE migration_attachment", NULL,
                       NULL, NULL) == SQLITE_OK);
    result = 0;
done:
    return result;
}
static int migration_recovery_target_case(sqlite3 *db, const gstd_deployment_target *target) {
    int result = 1;
    gstd_migration_execution observed = {0};
    CHECK(sqlite3_exec(db, "SAVEPOINT migration_recovery_target", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_migration_execution_recovery_read(db, target->operation_id, "recovery-stop",
                                                 &observed) < 0);
    sqlite3_stmt *s = NULL;
    CHECK(
        sqlite3_prepare_v2(
            db,
            "INSERT INTO resource_ownership SELECT 'cfg-'||?, "
            "'configuration',deployment_id,?,'allocated',"
            "json_set(backend_identity,'$.operationId',?,'$.generation',2),'uncertain',observed_at "
            "FROM resource_ownership WHERE resource_id=(SELECT "
            "'cfg-'||json_extract(backend_identity,'$.sourceOperationId') "
            "FROM resource_ownership WHERE resource_id='migration-'||?)",
            -1, &s, NULL) == SQLITE_OK);
    for (int i = 1; i <= 4; i++)
        sqlite3_bind_text(s, i, target->operation_id, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1);
    sqlite3_finalize(s);
    CHECK(sqlite3_exec(db,
                       "INSERT INTO operations SELECT "
                       "'recovery-stop',caller_uid,'deployment.stop',deployment_id,NULL,"
                       "printf('%064d',0),'{}',accepted_at,updated_at,'running','normal',NULL,NULL "
                       "FROM operations WHERE method='migration.start' AND state='running';"
                       "UPDATE operations SET state='failed',recovery='uncertain',error='{}' WHERE "
                       "method='migration.start';"
                       "UPDATE deployments SET "
                       "generation=3,last_operation_id='recovery-stop',recovery='uncertain';"
                       "UPDATE resource_ownership SET ownership_state='uncertain' WHERE "
                       "ownership_state='held'",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!gstd_migration_execution_recovery_read(db, target->operation_id, "recovery-stop",
                                                  &observed));
    CHECK(observed.target.generation == target->generation && observed.target.migration &&
          !observed.target.allow_provision &&
          json_equal(observed.target.manifest, target->manifest));
    CHECK(gstd_deployment_resource_reserve(db, &observed.target, "forbidden-recovery-scope",
                                           "workload") < 0);
    gstd_migration_execution_clear(&observed);
    CHECK(gstd_migration_execution_read(db, target->operation_id, &observed) < 0);
    CHECK(gstd_migration_execution_observe(db, target->operation_id, &observed) < 0);
    CHECK(gstd_migration_execution_recovery_read(db, target->operation_id, "foreign-stop",
                                                 &observed) < 0);
    CHECK(sqlite3_exec(db, "UPDATE deployments SET generation=2", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_migration_execution_recovery_read(db, target->operation_id, "recovery-stop",
                                                 &observed) < 0);
    CHECK(sqlite3_exec(db,
                       "UPDATE deployments SET last_operation_id=(SELECT operation_id FROM "
                       "operations WHERE method='deployment.start' LIMIT 1)",
                       NULL, NULL, NULL) == SQLITE_OK);
    uint64_t control_generation = 0;
    CHECK(!gstd_migration_recovery_claim(db, target->operation_id, "recovery-stop", &observed,
                                         &control_generation));
    CHECK(control_generation == 3 && observed.target.generation == 2);
    CHECK(gstd_migration_recovery_finish(db, &observed.target, "recovery-stop") < 0);
    gstd_migration_execution_clear(&observed);
    CHECK(!gstd_migration_recovery_claim(db, target->operation_id, "recovery-stop", &observed,
                                         &control_generation));
    CHECK(control_generation == 3);
    gstd_migration_execution_clear(&observed);
    CHECK(sqlite3_exec(db,
                       "ROLLBACK TO migration_recovery_target;RELEASE migration_recovery_target",
                       NULL, NULL, NULL) == SQLITE_OK);
    result = 0;
done:
    gstd_migration_execution_clear(&observed);
    return result;
}
static int service_fence_case(sqlite3 *db, const gstd_deployment_target *target) {
    int result = 1;
    char source[65], helper[65];
    json_t *value = NULL, *helper_value = NULL;
    CHECK(!resource_name(source, "run-scylladb-", target->operation_id));
    CHECK(!resource_name(helper, "run-schema-", target->operation_id));
    value = json_pack("{s:O,s:{s:s,s:s,s:s,s:I,s:i,s:i,s:s}}", "executionManifest",
                      target->manifest, "native", "kind", "libcrun", "id", source, "bootId",
                      "00000000-0000-0000-0000-000000000001", "generation",
                      (json_int_t)target->generation, "pid", 123, "startTime", 456, "component",
                      "scylladb");
    helper_value = json_pack("{s:O}", "executionManifest", target->manifest);
    gstd_native_context context = {.backend_id = source,
                                   .component = "scylladb",
                                   .boot_id = "00000000-0000-0000-0000-000000000001",
                                   .expected_pid = 123,
                                   .expected_start_time = 456,
                                   .control_owner = getpid()};
    CHECK(value && helper_value);
    CHECK(sqlite3_exec(db, "SAVEPOINT service_fence", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!gstd_deployment_resource_reserve(db, target, source, "workload"));
    CHECK(!gstd_deployment_resource_reserve(db, target, helper, "workload"));
    CHECK(!fence_identity(db, source, value, "running"));
    CHECK(!fence_identity(db, helper, helper_value, "reserved"));
    context.expected_start_time++;
    CHECK(gstd_migration_service_stopping(db, target, &context) < 0);
    context.expected_start_time--;
    CHECK(!gstd_migration_service_stopping(db, target, &context));
    CHECK(!gstd_migration_service_stopping(db, target, &context));
    CHECK(!fence_identity(db, source, value, "running"));
    json_t *control = json_pack("{s:i,s:s,s:i,s:s,s:b,s:n,s:n}", "purpose",
                                GSTD_EXEC_SCHEMA_MIGRATE, "sourceDatastore", source, "attempt", 1,
                                "stage", "dispatch", "unsafe", 1, "result", "termination");
    CHECK(control && !json_object_set_new(helper_value, "helperControl", control));
    CHECK(!fence_identity(db, helper, helper_value, "allocated"));
    CHECK(gstd_migration_service_stopping(db, target, &context) < 0);
    CHECK(!json_object_set_new(control, "stage", json_string("deleted")));
    CHECK(!fence_identity(db, helper, helper_value, "retained"));
    CHECK(gstd_migration_service_stopping(db, target, &context) < 0);
    CHECK(!json_object_del(helper_value, "helperControl"));
    CHECK(!json_object_set_new(helper_value, "native", json_object()));
    CHECK(!fence_identity(db, helper, helper_value, "allocated"));
    CHECK(gstd_migration_service_stopping(db, target, &context) < 0);
    CHECK(sqlite3_exec(db, "ROLLBACK TO service_fence;RELEASE service_fence", NULL, NULL, NULL) ==
          SQLITE_OK);
    result = 0;
done:
    json_decref(value);
    json_decref(helper_value);
    return result;
}
#ifdef __linux__
static int migration_asset_case(sqlite3 *db, const gstd_migration_execution *execution,
                                const char *root) {
    int result = 1, cache = -1, offline = -1;
    char path[512], resource[65], forbidden[65], key[65], staging[138];
    gstd_cache_owner owner = {.lock_fd = -1};
    snprintf(path, sizeof(path), "%s/assets", root);
    CHECK(!mkdir(path, 0700));
    cache = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    snprintf(path, sizeof(path), "%s/offline", root);
    CHECK(!mkdir(path, 0700));
    offline = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(cache >= 3 && offline >= 3);
    CHECK(gstd_asset_cache_lock(cache, execution->target.store_id, 0, 0, 1, &owner) ==
          GSTD_CACHE_OK);
    CHECK(!gstd_asset_record_cache_bind(db, &owner));
    CHECK(!resource_name(resource, "asset-schema-", execution->target.operation_id));
    CHECK(!resource_name(forbidden, "asset-git-", execution->target.operation_id));
    CHECK(!gstd_deployment_resource_reserve(db, &execution->target, resource, "asset-root"));
    CHECK(!gstd_deployment_resource_reserve(db, &execution->target, forbidden, "asset-root"));
    CHECK(gstd_asset_record_begin(db, execution->target.manifest, "api",
                                  execution->target.operation_id, resource, 32768, 65536, 16777216,
                                  key, staging) == GSTD_ASSET_RECORD_CONSENT);
    /* The selected owned offline directory authorizes preparation only. No SDK
     * job is run here; its contents are deliberately not claimed as OCI proof. */
    CHECK(gstd_asset_record_begin_offline(db, execution->target.manifest, offline, "api",
                                          execution->target.operation_id, resource, 32768, 65536,
                                          16777216, key, staging) == GSTD_ASSET_RECORD_NEW);
    gstd_provision_result unverified;
    CHECK(gstd_asset_record_receipt(db, key, execution->target.operation_id, resource,
                                    &unverified) < 0);
    CHECK(gstd_asset_record_begin_offline(db, execution->target.manifest, offline, "git",
                                          execution->target.operation_id, forbidden, 32768, 65536,
                                          16777216, key, staging) == GSTD_ASSET_RECORD_ERROR);
    CHECK(gstd_asset_record_begin_offline(db, execution->target.manifest, offline, "scylladb",
                                          execution->target.operation_id, resource, 32768, 65536,
                                          16777216, key, staging) == GSTD_ASSET_RECORD_ERROR);
    result = 0;
done:
    gstd_asset_cache_unlock(&owner);
    if (cache >= 0)
        close(cache);
    if (offline >= 0)
        close(offline);
    return result;
}
#endif
static int scenario(int outcome) {
    int result = 1;
    char *root = gstd_test_dir(), path[512], source[65], first[65], second[65], replay[65],
         taken[65];
    sqlite3 *db = NULL;
    json_t *request = NULL, *record = NULL, *components = json_pack("[s,s]", "api", "git"),
           *target = NULL;
    const char *version = "gitstore-06152d-schema-012";
    CHECK(root && components);
    snprintf(path, sizeof(path), "%s/state.db", root);
    CHECK(!gstd_state_open(path, &db));
#if defined(__aarch64__) || defined(__arm64__)
    const char *architecture = "arm64", *platform = "linux-arm64";
#else
    const char *architecture = "amd64", *platform = "linux-amd64";
#endif
    json_t *profile_manifest =
        gstd_asset_manifest_for_datastore(components, architecture, "scylladb");
    sqlite3_stmt *profile_statement = NULL;
    CHECK(profile_manifest &&
          sqlite3_prepare_v2(db, "INSERT INTO profiles VALUES('core','1',?,?,?)", -1,
                             &profile_statement, NULL) == SQLITE_OK);
    sqlite3_bind_text(profile_statement, 1,
                      json_string_value(json_object_get(profile_manifest, "serverRevision")), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(profile_statement, 2,
                      json_string_value(json_object_get(profile_manifest, "profileDigest")), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(profile_statement, 3, platform, -1, SQLITE_TRANSIENT);
    int stored_profile = sqlite3_step(profile_statement);
    sqlite3_finalize(profile_statement);
    json_decref(profile_manifest);
    CHECK(stored_profile == SQLITE_DONE);
    request = json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:s,s:s}}", "version", "major", 1,
                        "minor", 0, "requestId", "start", "method", "deployment.start",
                        "idempotencyKey", "source", "params", "deploymentId", "persistent",
                        "profileId", "core", "profileVersion", "1", "datastore", "scylladb");
    gstd_request parsed;
    CHECK(request && !gstd_request_parse(request, &parsed));
    CHECK(gstd_accept(db, &parsed, components, NULL, source) == GSTD_ACCEPT_NEW);
    CHECK(gstd_schedule_take(db, 0, taken) == 1 && !strcmp(taken, source));
    CHECK(sqlite3_exec(db,
                       "UPDATE operations SET state='succeeded',result='{}';UPDATE deployments SET "
                       "desired_state='stopped',writer_ownership='released',generation=2",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!gstd_schedule_finish(db, source));
    json_decref(request);
    request = NULL;
    target = gstd_migration_target(version);
    CHECK(target && !gstd_migration_target("unregistered"));
    CHECK(!strcmp(json_string_value(json_object_get(target, "datastore")), "scylladb"));
    request = message("missing-backup", version, 0);
    CHECK(request && accept(db, request, first) == GSTD_ACCEPT_MIGRATION_PRECONDITION);
    json_decref(request);
    request = message("unknown-target", "unregistered", 1);
    CHECK(request && accept(db, request, first) == GSTD_ACCEPT_MIGRATION_PRECONDITION);
    CHECK(scalar(db, "SELECT count(*) FROM operations WHERE method='migration.start'") == 0);
    json_decref(request);
    CHECK(sqlite3_exec(db,
                       "UPDATE operations SET intent=json_set(intent,'$.datastore','memdb') WHERE "
                       "method='deployment.start'",
                       NULL, NULL, NULL) == SQLITE_OK);
    request = message("unsupported", version, 1);
    CHECK(request && accept(db, request, first) == GSTD_ACCEPT_UNSUPPORTED_DATASTORE);
    json_decref(request);
    CHECK(sqlite3_exec(db,
                       "UPDATE operations SET intent=json_set(intent,'$.datastore','scylladb') "
                       "WHERE method='deployment.start'",
                       NULL, NULL, NULL) == SQLITE_OK);
    request = message("first", version, 1);
    CHECK(request && accept(db, request, first) == GSTD_ACCEPT_NEW);
    CHECK(gstd_migration_cancel_safe(db, first) == 1);
    CHECK(gstd_migration_cancel_queued(db, first) < 0); /* Transaction required. */
    CHECK(sqlite3_exec(db, "SAVEPOINT queued_cancel", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!gstd_migration_cancel_queued(db, first));
    CHECK(scalar(db, "SELECT count(*) FROM operations WHERE method='migration.start' AND "
                     "state='cancelled'") == 1);
    CHECK(scalar(db, "SELECT count(*) FROM resource_ownership WHERE resource_id LIKE 'migration-%' "
                     "AND ownership_state='held'") == 0);
    CHECK(sqlite3_exec(db, "ROLLBACK TO queued_cancel;RELEASE queued_cancel", NULL, NULL, NULL) ==
          SQLITE_OK);

    CHECK(gstd_migration_start_allowed(db, "persistent") == 0);
    json_decref(request);
    request = message("conflicting", version, 1);
    CHECK(request && accept(db, request, second) == GSTD_ACCEPT_MIGRATION_CONFLICT);
    CHECK(gstd_schedule_take(db, 0, taken) == 1 && !strcmp(taken, first));
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_migration_cancel_queued(db, first) <
          0); /* Running preparation is never cancelled here. */
    CHECK(sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);

    /* Policy loading remains metadata only. A stopped row without the actual
     * retained source configuration cannot become an execution target. */
    gstd_migration_execution execution = {0};
    CHECK(gstd_migration_execution_read(db, first, &execution) < 0);
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE;SAVEPOINT execution_policy", NULL, NULL, NULL) ==
          SQLITE_OK);
    CHECK(gstd_migration_execution_read(db, first, &execution) < 0);
    json_t *policy = gstd_asset_manifest_for_datastore(components, architecture, "scylladb");
    char *policy_bytes = policy ? gstd_json_dump(policy) : NULL;
    CHECK(policy_bytes);
    sqlite3_stmt *policy_statement = NULL;
    CHECK(sqlite3_prepare_v2(
              db,
              "INSERT INTO resource_ownership SELECT 'cfg-'||?, 'configuration',"
              "'persistent',?, 'retained',json_object('schemaVersion',1,'kind','configuration',"
              "'storeId',store_id,'deploymentId','persistent','operationId',?,'generation',1,"
              "'profileDigest',json_extract(?,'$.profileDigest'),'executionManifest',json(?)),"
              "'released','2026-11-05T00:00:00Z' FROM query_meta",
              -1, &policy_statement, NULL) == SQLITE_OK);
    for (int field = 1; field <= 3; field++)
        sqlite3_bind_text(policy_statement, field, source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(policy_statement, 4, policy_bytes, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(policy_statement, 5, policy_bytes, -1, SQLITE_TRANSIENT);
    int inserted = sqlite3_step(policy_statement);
    sqlite3_finalize(policy_statement);
    free(policy_bytes);
    json_decref(policy);
    CHECK(inserted == SQLITE_DONE);
    CHECK(!gstd_migration_execution_read(db, first, &execution));
    CHECK(!strcmp(execution.source_operation, source) &&
          !strcmp(execution.target.operation_id, first) && execution.target.generation == 2 &&
          execution.target.migration && !execution.target.allow_provision);
    CHECK(!migration_recovery_target_case(db, &execution.target));
    CHECK(!migration_attachment_case(db, &execution.target));
    CHECK(!service_fence_case(db, &execution.target));
    CHECK(!gstd_deployment_resource_reserve(db, &execution.target, "migration-prepared-scope",
                                            "configuration"));
    gstd_bootstrap_graph forbidden_graph;
    int forbidden_credential = 1;
    char read_helper[65], schema_helper[65];
    CHECK(!resource_name(read_helper, "run-cql-", first));
    CHECK(!resource_name(schema_helper, "run-schema-", first));
    CHECK(gstd_deployment_helper_reserve(db, &execution.target, "migration-prepared-scope",
                                         read_helper, GSTD_EXEC_SCYLLA_READY) < 0);
    CHECK(gstd_migration_helper_reserve(db, &execution.target, "migration-prepared-scope",
                                        read_helper, GSTD_EXEC_SCYLLA_KEYSPACE) < 0);
    CHECK(gstd_migration_helper_reserve(db, &execution.target, "migration-prepared-scope",
                                        read_helper, GSTD_EXEC_SCYLLA_READY) < 0);
    CHECK(gstd_deployment_graph_begin(db, &execution.target, "migration-prepared-scope",
                                      &forbidden_graph) < 0);
    CHECK(gstd_deployment_credential_reserve(db, &execution.target, "migration-prepared-scope",
                                             &forbidden_credential) < 0 &&
          !forbidden_credential);
    CHECK(sqlite3_exec(db, "SAVEPOINT substituted_target", NULL, NULL, NULL) == SQLITE_OK);
    execution.target.generation++;
    CHECK(gstd_deployment_resource_reserve(db, &execution.target, "foreign-scope", "workload") < 0);
    execution.target.generation--;
    CHECK(sqlite3_exec(db, "ROLLBACK TO substituted_target;RELEASE substituted_target", NULL, NULL,
                       NULL) == SQLITE_OK);
#ifdef __linux__
    CHECK(!migration_asset_case(db, &execution, root));
#endif
    CHECK(sqlite3_exec(db, "SAVEPOINT active_observer", NULL, NULL, NULL) == SQLITE_OK);
    gstd_migration_observation staged = {1, 1, 1, 1};
    CHECK(!gstd_migration_prepare(db, first, &staged));
    gstd_migration_execution observer = {0};
    CHECK(gstd_migration_execution_read(db, first, &observer) < 0);
    CHECK(!gstd_migration_execution_observe(db, first, &observer));
    CHECK(gstd_migration_helper_reserve(db, &observer.target, "migration-prepared-scope",
                                        schema_helper, GSTD_EXEC_SCHEMA_MIGRATE) < 0);
    json_t *unchanged = gstd_migration_inspect(db, first);
    CHECK(unchanged &&
          !strcmp(json_string_value(json_object_get(unchanged, "stage")), "prepared") &&
          json_is_false(json_object_get(unchanged, "unsafe")));
    json_decref(unchanged);
    CHECK(observer.target.generation == execution.target.generation &&
          json_equal(observer.target.manifest, execution.target.manifest));
    CHECK(gstd_deployment_resource_reserve(db, &observer.target, "too-late-scope", "workload") < 0);
    gstd_migration_execution_clear(&observer);
    CHECK(sqlite3_exec(db, "ROLLBACK TO active_observer;RELEASE active_observer", NULL, NULL,
                       NULL) == SQLITE_OK);
    gstd_migration_execution_clear(&execution);
    char absent_volume[65];
    CHECK(gstd_scylla_migration_volume_reserve(db, first, absent_volume) ==
              GSTD_ACCEPT_MIGRATION_PRECONDITION &&
          !*absent_volume);
    CHECK(scalar(db, "SELECT count(*) FROM datastore_volumes") == 0);
    CHECK(sqlite3_exec(db,
                       "SAVEPOINT execution_race;UPDATE deployments SET desired_state='running'",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_migration_execution_read(db, first, &execution) < 0);
    CHECK(sqlite3_exec(db, "ROLLBACK TO execution_race;UPDATE deployments SET generation=3", NULL,
                       NULL, NULL) == SQLITE_OK);
    CHECK(gstd_migration_execution_read(db, first, &execution) < 0);
    CHECK(sqlite3_exec(db,
                       "ROLLBACK TO execution_race;UPDATE resource_ownership SET "
                       "ownership_state='uncertain' WHERE ownership_state='released'",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_migration_execution_read(db, first, &execution) < 0);
    CHECK(sqlite3_exec(db, "ROLLBACK TO execution_race;RELEASE execution_race", NULL, NULL, NULL) ==
          SQLITE_OK);
    CHECK(sqlite3_exec(db,
                       "UPDATE resource_ownership SET backend_identity=json_set(backend_identity,"
                       "'$.executionManifest.migrationTarget','foreign') WHERE "
                       "resource_type='configuration' AND ownership_state='released'",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_migration_execution_read(db, first, &execution) < 0);
    CHECK(sqlite3_exec(db, "ROLLBACK TO execution_policy;RELEASE execution_policy;ROLLBACK", NULL,
                       NULL, NULL) == SQLITE_OK);
    gstd_migration_observation observation = {1, 1, 1, 1};
    observation.keyspace_exists = 0;
    CHECK(gstd_migration_check(db, first, &observation) == GSTD_ACCEPT_MIGRATION_PRECONDITION);
    observation.keyspace_exists = 1;
    observation.schema_compatible = 0;
    CHECK(gstd_migration_check(db, first, &observation) == GSTD_ACCEPT_MIGRATION_PRECONDITION);
    observation.schema_compatible = 1;
    observation.datastore_exclusive = 0;
    CHECK(gstd_migration_check(db, first, &observation) == GSTD_ACCEPT_MIGRATION_CONFLICT);
    observation.datastore_exclusive = 1;
    observation.previous_helper_terminated = 0;
    CHECK(gstd_migration_check(db, first, &observation) == GSTD_ACCEPT_OWNERSHIP_UNCERTAIN);
    observation.previous_helper_terminated = 1;
    CHECK(gstd_migration_check(db, first, &observation) == GSTD_ACCEPT_NEW);
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_migration_transition(db, first, GSTD_MIG_PREFLIGHT, GSTD_MIG_PREPARED) == -1);
    observation.datastore_exclusive = 0;
    CHECK(gstd_migration_prepare(db, first, &observation) == -1);
    observation.datastore_exclusive = 1;
    CHECK(!gstd_migration_prepare(db, first, &observation));
    CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    /* A real denied write cannot advance the persisted dispatch boundary. */
    CHECK(sqlite3_exec(db, "PRAGMA query_only=ON;BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_migration_transition(db, first, GSTD_MIG_PREPARED, GSTD_MIG_DISPATCHED) == -1);
    CHECK(sqlite3_exec(db, "ROLLBACK;PRAGMA query_only=OFF", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    CHECK(!gstd_state_open(path, &db));
    json_decref(request);
    request = message("first", version, 1);
    CHECK(request && accept(db, request, replay) == GSTD_ACCEPT_REPLAY && !strcmp(first, replay));
    record = gstd_migration_inspect(db, first);
    CHECK(record && !strcmp(json_string_value(json_object_get(record, "stage")), "prepared"));
    json_decref(record);
    record = NULL;
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    if (outcome == 2) {
        CHECK(!gstd_migration_finish(db, first, 0, 1));
        CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
        CHECK(!gstd_schedule_finish(db, first));
        CHECK(gstd_migration_start_allowed(db, "persistent") == 0);
        json_decref(request);
        request = message("safe-retry", version, 1);
        CHECK(request && accept(db, request, second) == GSTD_ACCEPT_NEW);
        result = 0;
        goto done;
    }
    CHECK(!gstd_migration_transition(db, first, GSTD_MIG_PREPARED, GSTD_MIG_DISPATCHED));
    CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_migration_cancel_safe(db, first) == 0);
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_migration_transition(db, first, GSTD_MIG_DISPATCHED, GSTD_MIG_PREPARED) == -1);
    if (outcome == 1) {
        CHECK(gstd_migration_finish(db, first, 1, 1) == -1);
        CHECK(!gstd_migration_transition(db, first, GSTD_MIG_DISPATCHED, GSTD_MIG_VERIFIED));
        CHECK(!gstd_migration_transition(db, first, GSTD_MIG_VERIFIED, GSTD_MIG_CLEANUP));
        CHECK(gstd_migration_finish(db, first, 1, 0) == -1);
        CHECK(!gstd_migration_finish(db, first, 1, 1));
        CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
        CHECK(!gstd_schedule_finish(db, first));
        CHECK(gstd_migration_start_allowed(db, "persistent") == 1);
        CHECK(sqlite3_exec(
                  db,
                  "SAVEPOINT corruption;UPDATE resource_ownership SET ownership_state='uncertain' "
                  "WHERE json_extract(backend_identity,'$.kind')='migration-control'",
                  NULL, NULL, NULL) == SQLITE_OK);
        CHECK(gstd_migration_start_allowed(db, "persistent") != 1);
        CHECK(sqlite3_exec(db,
                           "ROLLBACK TO corruption;RELEASE corruption;SAVEPOINT corruption;UPDATE "
                           "resource_ownership SET "
                           "backend_identity=json_set(backend_identity,'$.targetDigest','wrong') "
                           "WHERE json_extract(backend_identity,'$.kind')='migration-control'",
                           NULL, NULL, NULL) == SQLITE_OK);
        CHECK(gstd_migration_start_allowed(db, "persistent") == -1);
        CHECK(sqlite3_exec(db, "ROLLBACK TO corruption;RELEASE corruption", NULL, NULL, NULL) ==
              SQLITE_OK);
        CHECK(gstd_migration_start_allowed(db, "persistent") == 1);
        result = 0;
        goto done;
    }
    CHECK(!gstd_migration_finish(db, first, 0, 0));
    CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!gstd_schedule_finish(db, first));
    CHECK(gstd_migration_start_allowed(db, "persistent") == 0);
    json_t *blocked_start =
        json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:s,s:s}}", "version", "major", 1, "minor",
                  0, "requestId", "blocked", "method", "deployment.start", "idempotencyKey",
                  "post-migration", "params", "deploymentId", "persistent", "profileId", "core",
                  "profileVersion", "1", "datastore", "scylladb");
    gstd_request blocked_parsed;
    int blocked_valid = blocked_start && !gstd_request_parse(blocked_start, &blocked_parsed);
    gstd_accept_status blocked_status =
        blocked_valid ? gstd_accept(db, &blocked_parsed, components, NULL, second)
                      : GSTD_ACCEPT_INVALID;
    json_decref(blocked_start);
    CHECK(blocked_status == GSTD_ACCEPT_MIGRATION_PRECONDITION);
    CHECK(scalar(db, "SELECT count(*) FROM operations WHERE method='deployment.start'") == 1);
    record = gstd_migration_inspect(db, first);
    CHECK(record && !strcmp(json_string_value(json_object_get(record, "stage")), "uncertain"));
    json_decref(record);
    record = NULL;
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_migration_finish(db, first, 1, 1) == -1); /* terminal history is immutable */
    CHECK(sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(scalar(db, "SELECT count(*) FROM operations WHERE method='deployment.start' AND "
                     "state='succeeded' AND result='{}'") == 1);
    result = 0;
done:
    json_decref(request);
    json_decref(record);
    json_decref(components);
    json_decref(target);
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}

static int native_history_file(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat before, after;
    if (fd < 3 || fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_uid != geteuid() ||
        (before.st_mode & 0022) || before.st_nlink != 1 || before.st_size <= 0 ||
        before.st_size > 65536) {
        if (fd >= 0)
            close(fd);
        return 1;
    }
    unsigned char bytes[65536];
    ssize_t size = pread(fd, bytes, (size_t)before.st_size, 0);
    int stable = size == before.st_size && !fstat(fd, &after) && before.st_dev == after.st_dev &&
                 before.st_ino == after.st_ino && before.st_size == after.st_size &&
                 before.st_mtime == after.st_mtime && before.st_ctime == after.st_ctime;
    close(fd);
    json_t *sets = stable ? gstd_migration_cql_decode(bytes, (size_t)size, 1) : NULL;
    int valid = sets && !gstd_migration_history_validate("gitstore-06152d-schema-012",
                                                         json_array_get(sets, 0), 1);
    json_decref(sets);
    if (valid)
        puts("{\"nativeHistoryValidated\":true,\"rows\":12}");
    return valid ? 0 : 1;
}
int main(int argc, char **argv) {
    gstd_json_initialize();
    if (argc == 3 && !strcmp(argv[1], "--history-output"))
        return native_history_file(argv[2]);
    if (argc != 1)
        return 2;
    if (cql_decode_case() || schema_inspection_case() || history_case())
        return 1;
    for (int outcome = 0; outcome < 3; outcome++)
        if (scenario(outcome))
            return 1;
    return 0;
}
