#define _POSIX_C_SOURCE 200809L
#include "deployment_records.h"
#include "gitstored/assets.h"
#include "gitstored/profiles.h"
#include "gitstored/protocol.h"
#include "migration.h"
#include "migration_execution.h"
#include "native_exit.h"
#include "network_records.h"
#include "oidc_profile.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static sqlite3_stmt *prepare(sqlite3 *db, const char *text) {
    sqlite3_stmt *s = NULL;
    return sqlite3_prepare_v2(db, text, -1, &s, NULL) == SQLITE_OK ? s : NULL;
}
static void bind(sqlite3_stmt *s, int column, const char *text) {
    sqlite3_bind_text(s, column, text, -1, SQLITE_TRANSIENT);
}
static int identifier(const char *text) { return text && gstd_identifier(text, strnlen(text, 65)); }
static int column(sqlite3_stmt *s, int index, char *output, size_t size) {
    const unsigned char *text = sqlite3_column_text(s, index);
    int n = sqlite3_column_bytes(s, index);
    if (!text || n < 1 || (size_t)n >= size || memchr(text, 0, (size_t)n))
        return -1;
    memcpy(output, text, (size_t)n);
    output[n] = 0;
    return 0;
}
void gstd_deployment_target_clear(gstd_deployment_target *target) {
    if (!target)
        return;
    json_decref(target->manifest);
    json_decref(target->components);
    memset(target, 0, sizeof(*target));
}
static int target_read(sqlite3 *db, const char *op, gstd_deployment_target *output, int recovery) {
    if (!db || !identifier(op) || !output)
        return -1;
    const char *sql =
        recovery
            ? "SELECT o.intent,o.deployment_id,json_extract(r.backend_identity,'$.generation'),"
              "q.store_id,p.server_revision,p.manifest_digest,p.platform FROM operations o "
              "JOIN deployments d ON d.deployment_id=o.deployment_id "
              "JOIN profiles p ON p.profile_id='core' AND p.profile_version='1' "
              "JOIN resource_ownership r ON r.operation_id=o.operation_id AND "
              "r.deployment_id=d.deployment_id "
              "CROSS JOIN query_meta q WHERE o.operation_id=? AND o.caller_uid=? AND "
              "o.method='deployment.start' AND r.resource_id='cfg-'||o.operation_id AND "
              "r.resource_type='configuration' AND r.ownership_state IN ('held','uncertain') AND "
              "json_valid(r.backend_identity) AND "
              "json_extract(r.backend_identity,'$.schemaVersion')=1 AND "
              "json_type(r.backend_identity,'$.generation')='integer' AND "
              "json_extract(r.backend_identity,'$.kind')='configuration' AND "
              "json_extract(r.backend_identity,'$.storeId')=q.store_id AND "
              "json_extract(r.backend_identity,'$.operationId')=o.operation_id AND "
              "json_extract(r.backend_identity,'$.deploymentId')=d.deployment_id AND "
              "json_extract(r.backend_identity,'$.profileDigest')=p.manifest_digest"
            : "SELECT "
              "o.intent,o.deployment_id,d.generation,q.store_id,p.server_revision,p.manifest_"
              "digest,"
              "p.platform FROM operations o JOIN deployments d ON d.deployment_id=o.deployment_id "
              "JOIN profiles p ON p.profile_id=d.profile_id AND "
              "p.profile_version=d.profile_version "
              "CROSS JOIN query_meta q WHERE o.operation_id=? AND o.caller_uid=? AND "
              "o.method='deployment.start' AND d.last_operation_id=o.operation_id";
    sqlite3_stmt *s = prepare(db, sql);
    if (!s)
        return -1;
    bind(s, 1, op);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
    gstd_deployment_target target = {0};
    json_t *intent = NULL;
    char revision[65], digest[72], platform[64];
    int valid = sqlite3_step(s) == SQLITE_ROW;
    if (valid) {
        const unsigned char *bytes = sqlite3_column_text(s, 0);
        int size = sqlite3_column_bytes(s, 0);
        intent = bytes && size > 0 ? gstd_json_parse(bytes, (size_t)size) : NULL;
        sqlite3_int64 generation = sqlite3_column_int64(s, 2);
        valid = intent && generation > 0 && generation <= GSTD_SAFE_INTEGER &&
                !column(s, 1, target.deployment_id, sizeof(target.deployment_id)) &&
                identifier(target.deployment_id) &&
                !column(s, 3, target.store_id, sizeof(target.store_id)) &&
                strlen(target.store_id) == 32 && !column(s, 4, revision, sizeof(revision)) &&
                !column(s, 5, digest, sizeof(digest)) && !column(s, 6, platform, sizeof(platform));
        target.generation = (uint64_t)generation;
    }
    sqlite3_finalize(s);
    const char *profile = json_string_value(json_object_get(intent, "profileId")),
               *version = json_string_value(json_object_get(intent, "profileVersion"));
    const char *datastore = json_string_value(json_object_get(intent, "datastore"));
    if (!valid || !profile || strcmp(profile, "core") || !version || strcmp(version, "1") ||
        !datastore || (strcmp(datastore, "memdb") && strcmp(datastore, "scylladb")))
        goto failed;
    target.components = gstd_profile_components(json_object_get(intent, "components"));
#if defined(__aarch64__) || defined(__arm64__)
    const char *architecture = "arm64";
#else
    const char *architecture = "amd64";
#endif
    target.manifest = target.components ? gstd_asset_manifest_for_datastore(target.components,
                                                                            architecture, datastore)
                                        : NULL;
    char expected_platform[64];
    snprintf(expected_platform, sizeof(expected_platform), "linux-%s", architecture);
    if (!target.manifest || strcmp(platform, expected_platform) ||
        strcmp(revision, json_string_value(json_object_get(target.manifest, "serverRevision"))) ||
        strcmp(digest, json_string_value(json_object_get(target.manifest, "profileDigest"))))
        goto failed;
    memcpy(target.operation_id, op, strlen(op) + 1);
    target.allow_provision = !recovery && json_is_true(json_object_get(intent, "allowProvision"));
    json_decref(intent);
    *output = target;
    return 0;
failed:
    json_decref(intent);
    gstd_deployment_target_clear(&target);
    return -1;
}
int gstd_deployment_target_read(sqlite3 *db, const char *op, gstd_deployment_target *output) {
    return target_read(db, op, output, 0);
}
int gstd_deployment_target_recovery_read(sqlite3 *db, const char *op,
                                         gstd_deployment_target *output) {
    return target_read(db, op, output, 1);
}
static int current_phase(sqlite3 *db, const gstd_deployment_target *target, int observation) {
    if (!db || sqlite3_get_autocommit(db) || !target || !identifier(target->operation_id) ||
        !identifier(target->deployment_id) || !target->generation ||
        gstd_asset_manifest_check(target->manifest))
        return 0;
    if (target->migration) {
        gstd_migration_execution execution = {0};
        int valid =
            target->migration == 1 && !target->allow_provision &&
            !(observation ? gstd_migration_execution_observe(db, target->operation_id, &execution)
                          : gstd_migration_execution_read(db, target->operation_id, &execution)) &&
            !strcmp(target->deployment_id, execution.target.deployment_id) &&
            !strcmp(target->store_id, execution.target.store_id) &&
            target->generation == execution.target.generation &&
            json_equal(target->manifest, execution.target.manifest) &&
            json_equal(target->components, execution.target.components);
        gstd_migration_execution_clear(&execution);
        return valid;
    }
    sqlite3_stmt *s = prepare(
        db, "SELECT 1 FROM deployments d JOIN operations o ON o.operation_id=d.last_operation_id "
            "CROSS JOIN query_meta q WHERE d.deployment_id=? AND d.generation=? AND "
            "o.operation_id=? AND o.caller_uid=? AND o.method='deployment.start' AND q.store_id=?");
    if (!s)
        return 0;
    bind(s, 1, target->deployment_id);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)target->generation);
    bind(s, 3, target->operation_id);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)geteuid());
    bind(s, 5, target->store_id);
    int ok = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    return ok;
}
static int current(sqlite3 *db, const gstd_deployment_target *target) {
    return current_phase(db, target, 0);
}
int gstd_deployment_resource_reserve(sqlite3 *db, const gstd_deployment_target *target,
                                     const char *id, const char *kind) {
    if (!current(db, target) || !identifier(id) || !kind ||
        (strcmp(kind, "configuration") && strcmp(kind, "network") && strcmp(kind, "workload") &&
         strcmp(kind, "asset-root")))
        return -1;
    sqlite3_stmt *s =
        prepare(db, "INSERT INTO resource_ownership "
                    "VALUES(?,?,?,?,'reserved',NULL,'held',strftime('%Y-%m-%dT%H:%M:%SZ','now'))");
    if (!s)
        return -1;
    bind(s, 1, id);
    bind(s, 2, kind);
    bind(s, 3, target->deployment_id);
    bind(s, 4, target->operation_id);
    int ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return ok ? 0 : -1;
}
static json_t *owner_phase(sqlite3 *db, const gstd_deployment_target *target, const char *id,
                           const char *kind, int observation) {
    if (!current_phase(db, target, observation) || !identifier(id))
        return NULL;
    sqlite3_stmt *s = prepare(
        db, "SELECT backend_identity FROM resource_ownership WHERE resource_id=? AND "
            "resource_type=? AND deployment_id=? AND operation_id=? AND ownership_state='held'");
    if (!s)
        return NULL;
    bind(s, 1, id);
    bind(s, 2, kind);
    bind(s, 3, target->deployment_id);
    bind(s, 4, target->operation_id);
    json_t *value = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const unsigned char *bytes = sqlite3_column_text(s, 0);
        int n = sqlite3_column_bytes(s, 0);
        value = bytes && n > 0 ? gstd_json_parse(bytes, (size_t)n) : json_object();
    }
    sqlite3_finalize(s);
    /* Persistent execution policy is immutable for this producing operation.
     * In particular, a later catalogue must not reinterpret an old successful
     * schema helper as execution of a new target while reusing image caches. */
    if (value && json_object_size(value) &&
        (json_object_get(target->manifest, "datastore") ||
         json_object_get(target->manifest, "baseProfileDigest")) &&
        !json_equal(json_object_get(value, "executionManifest"), target->manifest)) {
        json_decref(value);
        value = NULL;
    }
    return value;
}
static json_t *owner(sqlite3 *db, const gstd_deployment_target *target, const char *id,
                     const char *kind) {
    return owner_phase(db, target, id, kind, 0);
}
static int write_owner(sqlite3 *db, const gstd_deployment_target *target, const char *id,
                       json_t *value, const char *stage) {
    char *bytes = gstd_json_dump(value);
    if (!bytes || strlen(bytes) > 32768) {
        free(bytes);
        return -1;
    }
    sqlite3_stmt *s = prepare(
        db,
        "UPDATE resource_ownership SET "
        "backend_identity=?,lifecycle_stage=?,observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') "
        "WHERE resource_id=? AND deployment_id=? AND operation_id=? AND ownership_state='held'");
    int ok = s != NULL;
    if (ok) {
        bind(s, 1, bytes);
        bind(s, 2, stage);
        bind(s, 3, id);
        bind(s, 4, target->deployment_id);
        bind(s, 5, target->operation_id);
        ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    }
    sqlite3_finalize(s);
    free(bytes);
    return ok ? 0 : -1;
}
int gstd_deployment_scope_record(sqlite3 *db, const gstd_deployment_target *target, const char *id,
                                 const gstd_filesystem_reply *receipt) {
    if (!current(db, target) || !identifier(id) || !receipt || receipt->status ||
        receipt->descriptor_count < 2 || receipt->descriptor_count > 3)
        return -1;
    struct stat info, data;
    if (fstat(receipt->descriptors[0], &info) || fstat(receipt->descriptors[1], &data) ||
        !S_ISDIR(info.st_mode) || !S_ISDIR(data.st_mode) || info.st_uid != geteuid() ||
        data.st_uid != geteuid() || (info.st_mode & 07777) != 0700 ||
        (data.st_mode & 07777) != 0700 || (uint64_t)info.st_dev != receipt->scope_device ||
        (uint64_t)info.st_ino != receipt->scope_inode)
        return -1;
    sqlite3_stmt *s =
        prepare(db, "SELECT resource_type FROM resource_ownership WHERE resource_id=? AND "
                    "deployment_id=? AND operation_id=? AND ownership_state='held' AND "
                    "lifecycle_stage='reserved' AND backend_identity IS NULL");
    if (!s)
        return -1;
    bind(s, 1, id);
    bind(s, 2, target->deployment_id);
    bind(s, 3, target->operation_id);
    char kind[32];
    int valid = sqlite3_step(s) == SQLITE_ROW && !column(s, 0, kind, sizeof(kind));
    sqlite3_finalize(s);
    if (!valid || (strcmp(kind, "configuration") && strcmp(kind, "workload")) ||
        receipt->descriptor_count != (unsigned)(!strcmp(kind, "workload") ? 3 : 2))
        return -1;
    char device[32], inode[32], data_device[32], data_inode[32];
    snprintf(device, sizeof(device), "%llu", (unsigned long long)info.st_dev);
    snprintf(inode, sizeof(inode), "%llu", (unsigned long long)info.st_ino);
    snprintf(data_device, sizeof(data_device), "%llu", (unsigned long long)data.st_dev);
    snprintf(data_inode, sizeof(data_inode), "%llu", (unsigned long long)data.st_ino);
    json_t *value = json_pack(
        "{s:i,s:s,s:s,s:s,s:s,s:I,s:s,s:s,s:s,s:s,s:s}", "schemaVersion", 1, "kind", kind,
        "storeId", target->store_id, "operationId", target->operation_id, "deploymentId",
        target->deployment_id, "generation", (json_int_t)target->generation, "profileDigest",
        json_string_value(json_object_get(target->manifest, "profileDigest")), "scopeDevice",
        device, "scopeInode", inode, "dataDevice", data_device, "dataInode", data_inode);
    if (value &&
        (json_object_get(target->manifest, "datastore") ||
         json_object_get(target->manifest, "baseProfileDigest")) &&
        json_object_set(value, "executionManifest", target->manifest)) {
        json_decref(value);
        value = NULL;
    }
    int result = value ? write_owner(db, target, id, value,
                                     !strcmp(kind, "workload") ? "reserved" : "allocated")
                       : -1;
    json_decref(value);
    return result;
}
static int identity_equals(const json_t *value, const char *field, uint64_t number) {
    char expected[32];
    snprintf(expected, sizeof(expected), "%llu", (unsigned long long)number);
    json_t *v = json_object_get(value, field);
    return json_is_string(v) && json_string_length(v) == strlen(expected) &&
           !memcmp(json_string_value(v), expected, strlen(expected));
}
int gstd_deployment_execution_record(sqlite3 *db, const gstd_deployment_target *target,
                                     const char *id, const gstd_filesystem_reply *scope,
                                     int rootfs) {
    if (!scope || scope->status || scope->descriptor_count != 3 || rootfs < 3)
        return -1;
    json_t *value = owner(db, target, id, "workload");
    sqlite3_stmt *phase =
        value
            ? prepare(db, "SELECT 1 FROM resource_ownership WHERE resource_id=? AND operation_id=? "
                          "AND lifecycle_stage='reserved' AND ownership_state='held'")
            : NULL;
    int reserved = 0;
    if (phase) {
        bind(phase, 1, id);
        bind(phase, 2, target->operation_id);
        reserved = sqlite3_step(phase) == SQLITE_ROW;
    }
    sqlite3_finalize(phase);
    struct stat wrapper, data, bundle, root, named;
    int valid = reserved && value && !json_object_get(value, "native") &&
                !json_object_get(value, "population") && !fstat(scope->descriptors[0], &wrapper) &&
                !fstat(scope->descriptors[1], &data) && !fstat(scope->descriptors[2], &bundle) &&
                !fstat(rootfs, &root);
    const struct stat *all[] = {&wrapper, &data, &bundle, &root};
    for (unsigned i = 0; valid && i < 4; i++)
        valid =
            S_ISDIR(all[i]->st_mode) && all[i]->st_uid == geteuid() && !(all[i]->st_mode & 0022);
    valid = valid && identity_equals(value, "scopeDevice", wrapper.st_dev) &&
            identity_equals(value, "scopeInode", wrapper.st_ino) &&
            identity_equals(value, "dataDevice", data.st_dev) &&
            identity_equals(value, "dataInode", data.st_ino) &&
            !fstatat(scope->descriptors[0], "execution", &named, AT_SYMLINK_NOFOLLOW) &&
            S_ISDIR(named.st_mode) && named.st_dev == data.st_dev && named.st_ino == data.st_ino &&
            !fstatat(scope->descriptors[0], "bundle", &named, AT_SYMLINK_NOFOLLOW) &&
            S_ISDIR(named.st_mode) && named.st_dev == bundle.st_dev &&
            named.st_ino == bundle.st_ino &&
            !fstatat(scope->descriptors[1], "rootfs", &named, AT_SYMLINK_NOFOLLOW) &&
            S_ISDIR(named.st_mode) && named.st_dev == root.st_dev && named.st_ino == root.st_ino;
    json_t *record = valid ? json_object() : NULL;
    char bytes[32];
    const char *fields[] = {"bundleDevice", "bundleInode", "rootfsDevice", "rootfsInode"};
    uint64_t numbers[] = {valid ? (uint64_t)bundle.st_dev : 0, valid ? (uint64_t)bundle.st_ino : 0,
                          valid ? (uint64_t)root.st_dev : 0, valid ? (uint64_t)root.st_ino : 0};
    for (unsigned i = 0; record && i < 4; i++) {
        snprintf(bytes, sizeof(bytes), "%llu", (unsigned long long)numbers[i]);
        if (json_object_set_new(record, fields[i], json_string(bytes)))
            valid = 0;
    }
    json_t *prior = json_object_get(value, "execution");
    valid = valid && record && (!prior || json_equal(prior, record)) &&
            !json_object_set(value, "execution", record);
    int result = valid ? write_owner(db, target, id, value, "reserved") : -1;
    json_decref(record);
    json_decref(value);
    return result;
}
static int helper_text(const json_t *object, const char *key, const char *expected) {
    json_t *value = json_object_get(object, key);
    return expected && json_is_string(value) && json_string_length(value) == strlen(expected) &&
           !memcmp(json_string_value(value), expected, strlen(expected));
}
int gstd_deployment_native_record(sqlite3 *db, const gstd_deployment_target *target,
                                  const gstd_native_context *context, const char *stage) {
    const gstd_execution_inputs *input = context ? context->registered_inputs : NULL;
    if (!context || !input || !stage || (strcmp(stage, "allocated") && strcmp(stage, "running")) ||
        !identifier(context->backend_id) || !context->component || !context->boot_id ||
        strlen(context->boot_id) != 36 || context->control_owner != getpid() ||
        input->purpose != GSTD_EXEC_SERVICE || !input->component ||
        strcmp(input->component, context->component) || !target ||
        (target->migration && strcmp(context->component, "scylladb")) ||
        !json_equal(input->manifest, target->manifest) || context->expected_pid < 0 ||
        (!!context->expected_pid != !!context->expected_start_time) ||
        (!strcmp(stage, "running") && !context->expected_pid))
        return -1;
    char expected[65];
    int n =
        snprintf(expected, sizeof(expected), "run-%s-%s", context->component, target->operation_id);
    int registered = 0;
    json_t *images = json_object_get(target->manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++)
        if (helper_text(json_array_get(images, i), "component", context->component))
            registered = 1;
    if (!registered || n < 1 || n >= (int)sizeof(expected) ||
        strcmp(expected, context->backend_id) || !input->network ||
        strcmp(context->boot_id, input->network->boot_id))
        return -1;
    json_t *value = owner(db, target, context->backend_id, "workload"),
           *execution = json_object_get(value, "execution"),
           *prior = json_object_get(value, "native");
    struct stat root, bundle;
    int valid =
        value && !json_object_get(value, "helperControl") &&
        !gstd_network_attachment_matches(db, target->operation_id, context->backend_id,
                                         input->network) &&
        !fstat(context->rootfs_fd, &root) && !fstat(context->bundle_fd, &bundle) &&
        identity_equals(execution, "rootfsDevice", root.st_dev) &&
        identity_equals(execution, "rootfsInode", root.st_ino) &&
        identity_equals(execution, "bundleDevice", bundle.st_dev) &&
        identity_equals(execution, "bundleInode", bundle.st_ino) &&
        (!prior ||
         (helper_text(prior, "kind", "libcrun") && helper_text(prior, "id", context->backend_id) &&
          helper_text(prior, "component", context->component) &&
          helper_text(prior, "bootId", context->boot_id) &&
          json_integer_value(json_object_get(prior, "generation")) ==
              (json_int_t)target->generation &&
          (!json_integer_value(json_object_get(prior, "pid")) ||
           (json_integer_value(json_object_get(prior, "pid")) == context->expected_pid &&
            json_integer_value(json_object_get(prior, "startTime")) ==
                (json_int_t)context->expected_start_time))));
    json_t *native =
        valid ? json_pack("{s:s,s:s,s:s,s:I,s:I,s:s,s:I}", "kind", "libcrun", "id",
                          context->backend_id, "bootId", context->boot_id, "pid",
                          (json_int_t)context->expected_pid, "startTime",
                          (json_int_t)context->expected_start_time, "component", context->component,
                          "generation", (json_int_t)target->generation)
              : NULL;
    valid = native && !json_object_set(value, "native", native) &&
            !write_owner(db, target, context->backend_id, value, stage);
    json_decref(native);
    json_decref(value);
    return valid ? 0 : -1;
}
int gstd_deployment_population_record(sqlite3 *db, const gstd_deployment_target *target,
                                      const gstd_native_context *context,
                                      const gstd_native_population *population) {
    if (!context || context->control_owner != getpid() || context->expected_pid <= 0 ||
        !context->expected_start_time || !context->component ||
        gstd_native_population_matches(population, context->backend_id, context->expected_pid,
                                       context->expected_start_time, context->boot_id))
        return -1;
    json_t *value = owner_phase(db, target, context->backend_id, "workload", 1),
           *native = json_object_get(value, "native"),
           *prior = json_object_get(value, "population");
    json_t *record =
        value ? gstd_native_population_identity(population, context->backend_id, context->boot_id)
              : NULL;
    int valid = record && helper_text(native, "kind", "libcrun") &&
                helper_text(native, "id", context->backend_id) &&
                helper_text(native, "component", context->component) &&
                helper_text(native, "bootId", context->boot_id) &&
                json_integer_value(json_object_get(native, "generation")) ==
                    (json_int_t)target->generation &&
                json_integer_value(json_object_get(native, "pid")) == context->expected_pid &&
                json_integer_value(json_object_get(native, "startTime")) ==
                    (json_int_t)context->expected_start_time &&
                (!prior || json_equal(prior, record));
    sqlite3_stmt *s =
        valid ? prepare(db,
                        "SELECT 1 FROM resource_ownership WHERE resource_id=? AND "
                        "operation_id=? AND lifecycle_stage='allocated' AND ownership_state='held'")
              : NULL;
    valid = s != NULL;
    if (s) {
        bind(s, 1, context->backend_id);
        bind(s, 2, target->operation_id);
        valid = sqlite3_step(s) == SQLITE_ROW;
    }
    sqlite3_finalize(s);
    int result = valid && !json_object_set(value, "population", record)
                     ? write_owner(db, target, context->backend_id, value, "allocated")
                     : -1;
    json_decref(record);
    json_decref(value);
    return result;
}
static int helper_name(const gstd_deployment_target *target, const char *id,
                       gstd_execution_purpose purpose, char source[65]) {
    char expected[65];
    if (!target || purpose < GSTD_EXEC_SCYLLA_READY || purpose > GSTD_EXEC_SCHEMA_HISTORY ||
        (target->migration != 0 && target->migration != 1) ||
        (!target->migration && purpose > GSTD_EXEC_SCHEMA_MIGRATE) ||
        (target->migration && purpose == GSTD_EXEC_SCYLLA_KEYSPACE))
        return 0;
    int n = snprintf(expected, sizeof(expected), "%s-%s",
                     purpose == GSTD_EXEC_SCHEMA_MIGRATE ? "run-schema" : "run-cql",
                     target->operation_id);
    int m = snprintf(source, 65, "run-scylladb-%s", target->operation_id);
    return n > 0 && n < (int)sizeof(expected) && m > 0 && m < 65 && id && !strcmp(expected, id);
}
static int helper_control_valid(const json_t *value, const char *source) {
    json_t *control = json_object_get(value, "helperControl"),
           *termination = json_object_get(control, "termination"),
           *result = json_object_get(control, "result");
    json_int_t purpose = json_integer_value(json_object_get(control, "purpose")),
               attempt = json_integer_value(json_object_get(control, "attempt"));
    int deleted = helper_text(control, "stage", "deleted");
    if (!json_is_object(control) || json_object_size(control) != 7 ||
        !json_is_integer(json_object_get(control, "purpose")) || purpose < GSTD_EXEC_SCYLLA_READY ||
        purpose > GSTD_EXEC_SCHEMA_HISTORY ||
        !json_is_integer(json_object_get(control, "attempt")) || attempt < 1 || attempt > 128 ||
        !helper_text(control, "sourceDatastore", source) ||
        !json_is_boolean(json_object_get(control, "unsafe")) ||
        json_is_true(json_object_get(control, "unsafe")) !=
            (!deleted &&
             (purpose == GSTD_EXEC_SCYLLA_KEYSPACE || purpose == GSTD_EXEC_SCHEMA_MIGRATE)))
        return 0;
    json_t *native = json_object_get(value, "native");
    if (!helper_text(control, "stage", "dispatch") &&
        (!helper_text(native, "kind", "libcrun") ||
         !identifier(json_string_value(json_object_get(native, "id"))) ||
         json_string_length(json_object_get(native, "bootId")) != 36 ||
         json_integer_value(json_object_get(native, "pid")) <= 0 ||
         json_integer_value(json_object_get(native, "startTime")) <= 0))
        return 0;
    if (deleted)
        return json_is_object(termination) && json_object_size(termination) == 6 &&
               json_is_string(json_object_get(termination, "cgroupDevice")) &&
               json_is_string(json_object_get(termination, "cgroupInode")) &&
               json_is_integer(json_object_get(termination, "exitCode")) &&
               json_is_boolean(json_object_get(termination, "signalled")) &&
               json_is_integer(result) && json_integer_value(result) >= -1 &&
               json_integer_value(result) <= 0 &&
               helper_text(termination, "kind", "helper-native-delete") &&
               json_equal(json_object_get(termination, "native"), json_object_get(value, "native"));
    return json_is_null(result) && json_is_null(termination) &&
           ((helper_text(control, "stage", "dispatch") && !json_object_get(value, "native") &&
             !json_object_get(value, "population")) ||
            (helper_text(control, "stage", "created") &&
             json_is_object(json_object_get(value, "native"))));
}
int gstd_deployment_helper_receipt_read(const gstd_deployment_target *target, const char *id,
                                        const json_t *value, const char *boot, int native_fd) {
    json_t *control = json_object_get(value, "helperControl"),
           *native = json_object_get(value, "native"),
           *termination = json_object_get(control, "termination");
    gstd_execution_purpose purpose =
        (gstd_execution_purpose)json_integer_value(json_object_get(control, "purpose"));
    char source[65];
    if (!target || !boot || strlen(boot) != 36 || native_fd < 3 ||
        !helper_name(target, id, purpose, source) || !helper_control_valid(value, source) ||
        !helper_text(control, "stage", "deleted") ||
        !json_equal(json_object_get(value, "executionManifest"), target->manifest) ||
        !helper_text(value, "storeId", target->store_id) ||
        !helper_text(value, "operationId", target->operation_id) ||
        !helper_text(value, "deploymentId", target->deployment_id) ||
        json_integer_value(json_object_get(value, "generation")) !=
            (json_int_t)target->generation ||
        !json_is_object(native) || json_object_size(native) != 7 ||
        !helper_text(native, "id", id) || !helper_text(native, "bootId", boot) ||
        !helper_text(native, "component",
                     purpose == GSTD_EXEC_SCHEMA_MIGRATE ? "api" : "scylladb") ||
        json_integer_value(json_object_get(native, "generation")) != (json_int_t)target->generation)
        return -1;
    json_int_t pid = json_integer_value(json_object_get(native, "pid")),
               start = json_integer_value(json_object_get(native, "startTime")),
               exit = json_integer_value(json_object_get(termination, "exitCode")),
               result = json_integer_value(json_object_get(control, "result"));
    int signalled = json_is_true(json_object_get(termination, "signalled"));
    if (pid <= 0 || pid > INT_MAX || start <= 0 || start > GSTD_SAFE_INTEGER || exit < 0 ||
        exit > 255 || (signalled && !exit) || result != (!signalled && !exit ? 0 : -1))
        return -1;
    const char *fields[] = {"cgroupDevice", "cgroupInode"};
    for (unsigned i = 0; i < 2; i++) {
        json_t *field = json_object_get(termination, fields[i]);
        const char *text = json_string_value(field);
        size_t length = json_string_length(field);
        if (!text || !length || length > 20 || text[0] == '0' || strlen(text) != length)
            return -1;
        for (size_t j = 0; j < length; j++)
            if (text[j] < '0' || text[j] > '9')
                return -1;
        errno = 0;
        (void)strtoull(text, NULL, 10);
        if (errno)
            return -1;
    }
    struct stat state;
    if (!fstatat(native_fd, id, &state, AT_SYMLINK_NOFOLLOW) || errno != ENOENT)
        return -1;
    uint64_t current = 0;
    errno = 0;
    if (!gstd_native_process_start((pid_t)pid, &current))
        return current == (uint64_t)start ? -1 : 0;
    return errno == ENOENT || errno == ESRCH ? 0 : -1;
}
int gstd_deployment_helper_reserve(sqlite3 *db, const gstd_deployment_target *target,
                                   const char *configuration, const char *id,
                                   gstd_execution_purpose purpose) {
    char source[65];
    if (!target || target->migration || !helper_name(target, id, purpose, source) ||
        !helper_text(target->manifest, "datastore", "scylladb"))
        return -1;
    json_t *value = owner(db, target, id, "workload"),
           *config = owner(db, target, configuration, "configuration"),
           *datastore = owner(db, target, source, "workload"),
           *graph = json_object_get(config, "graph"),
           *native = json_object_get(datastore, "native"),
           *control = json_object_get(value, "helperControl");
    int stage = purpose == GSTD_EXEC_SCYLLA_READY      ? GSTD_BOOT_WAIT_DATASTORE
                : purpose == GSTD_EXEC_SCYLLA_KEYSPACE ? GSTD_BOOT_PREPARE_KEYSPACE
                                                       : GSTD_BOOT_MIGRATE_SCHEMA;
    int valid = value && config && datastore &&
                json_object_size(json_object_get(value, "execution")) == 4 &&
                json_is_true(json_object_get(graph, "persistent")) &&
                json_is_integer(json_object_get(graph, "stage")) &&
                json_integer_value(json_object_get(graph, "stage")) == stage &&
                json_integer_value(json_object_get(graph, "generation")) ==
                    (json_int_t)target->generation &&
                json_is_integer(json_object_get(graph, "outcome")) &&
                !json_integer_value(json_object_get(graph, "outcome")) &&
                (purpose == GSTD_EXEC_SCYLLA_READY ||
                 json_is_true(json_object_get(graph, "freshDatastore"))) &&
                helper_text(native, "kind", "libcrun") && helper_text(native, "id", source) &&
                helper_text(native, "component", "scylladb") &&
                json_string_length(json_object_get(native, "bootId")) == 36 &&
                json_integer_value(json_object_get(native, "generation")) ==
                    (json_int_t)target->generation &&
                json_integer_value(json_object_get(native, "pid")) > 0 &&
                json_integer_value(json_object_get(native, "startTime")) > 0;
    sqlite3_stmt *s = valid
                          ? prepare(db, "SELECT 1 FROM operations o JOIN deployments d ON "
                                        "d.last_operation_id=o.operation_id WHERE o.operation_id=? "
                                        "AND o.state IN ('accepted','running') AND "
                                        "d.desired_state='running' AND d.recovery='normal' AND "
                                        "d.writer_ownership='held' AND EXISTS(SELECT 1 FROM "
                                        "resource_ownership WHERE resource_id=? AND "
                                        "operation_id=o.operation_id AND "
                                        "resource_type='workload' AND ownership_state='held' AND "
                                        "lifecycle_stage='running') AND "
                                        "EXISTS(SELECT 1 FROM resource_ownership r JOIN "
                                        "datastore_volumes v ON v.volume_id=r.resource_id WHERE "
                                        "r.operation_id=o.operation_id AND "
                                        "r.deployment_id=o.deployment_id AND "
                                        "r.resource_type='datastore-volume' AND "
                                        "r.ownership_state='held' AND v.allocated=1)")
                          : NULL;
    valid = s != NULL;
    if (s) {
        bind(s, 1, target->operation_id);
        bind(s, 2, source);
        valid = sqlite3_step(s) == SQLITE_ROW;
    }
    sqlite3_finalize(s);
    json_int_t attempt = 1;
    if (valid && control) {
        valid = helper_control_valid(value, source) && helper_text(control, "stage", "deleted") &&
                helper_text(json_object_get(value, "native"), "id", id) &&
                json_integer_value(json_object_get(control, "purpose")) == GSTD_EXEC_SCYLLA_READY &&
                purpose != GSTD_EXEC_SCHEMA_MIGRATE &&
                json_integer_value(json_object_get(control, "attempt")) < 128 &&
                (purpose != GSTD_EXEC_SCYLLA_KEYSPACE ||
                 json_integer_value(json_object_get(control, "result")) == 0);
        if (valid)
            attempt = json_integer_value(json_object_get(control, "attempt")) + 1;
    } else if (valid && (json_object_get(value, "native") || json_object_get(value, "population")))
        valid = 0;
    if (valid && purpose == GSTD_EXEC_SCYLLA_KEYSPACE && !control)
        valid = 0;
    if (valid && purpose == GSTD_EXEC_SCHEMA_MIGRATE) {
        char cql_id[65];
        int n = snprintf(cql_id, sizeof(cql_id), "run-cql-%s", target->operation_id);
        json_t *cql =
            n > 0 && n < (int)sizeof(cql_id) ? owner(db, target, cql_id, "workload") : NULL;
        json_t *keyspace = json_object_get(cql, "helperControl");
        valid =
            cql && helper_control_valid(cql, source) && helper_text(keyspace, "stage", "deleted") &&
            json_integer_value(json_object_get(keyspace, "purpose")) == GSTD_EXEC_SCYLLA_KEYSPACE &&
            json_integer_value(json_object_get(keyspace, "result")) == 0;
        json_decref(cql);
    }
    json_t *next =
        valid ? json_pack("{s:i,s:I,s:s,s:b,s:s,s:n,s:n}", "purpose", (int)purpose, "attempt",
                          attempt, "stage", "dispatch", "unsafe", purpose != GSTD_EXEC_SCYLLA_READY,
                          "sourceDatastore", source, "termination", "result")
              : NULL;
    valid = next &&
            (!control || !json_object_set(value, "previousHelperTermination",
                                          json_object_get(control, "termination"))) &&
            !json_object_set(value, "helperControl", next);
    if (valid)
        json_object_del(value, "native");
    if (valid)
        json_object_del(value, "population");
    int result = valid ? write_owner(db, target, id, value, "reserved") : -1;
    json_decref(next);
    json_decref(value);
    json_decref(config);
    json_decref(datastore);
    return result;
}
int gstd_migration_helper_reserve(sqlite3 *db, const gstd_deployment_target *target,
                                  const char *configuration, const char *id,
                                  gstd_execution_purpose purpose) {
    char source[65];
    if (!target || !target->migration || !helper_name(target, id, purpose, source))
        return -1;
    json_t *journal = gstd_migration_inspect(db, target->operation_id);
    int preflight = helper_text(journal, "stage", "preflight");
    int mutator = purpose == GSTD_EXEC_SCHEMA_MIGRATE;
    int after_mutation =
        purpose == GSTD_EXEC_SCHEMA_HISTORY && helper_text(journal, "stage", "dispatched");
    int allowed = mutator ? helper_text(journal, "stage", "prepared") : preflight || after_mutation;
    json_t *value = allowed ? owner_phase(db, target, id, "workload", 1) : NULL;
    json_t *config = value ? owner_phase(db, target, configuration, "configuration", 1) : NULL;
    json_t *datastore = config ? owner_phase(db, target, source, "workload", 1) : NULL;
    json_t *native = json_object_get(datastore, "native"),
           *control = json_object_get(value, "helperControl");
    int valid = value && config && datastore && !json_object_get(config, "graph") &&
                json_object_get(config, "scopeInode") &&
                json_object_size(json_object_get(value, "execution")) == 4 &&
                helper_text(native, "kind", "libcrun") && helper_text(native, "id", source) &&
                helper_text(native, "component", "scylladb") &&
                json_string_length(json_object_get(native, "bootId")) == 36 &&
                json_integer_value(json_object_get(native, "generation")) ==
                    (json_int_t)target->generation &&
                json_integer_value(json_object_get(native, "pid")) > 0 &&
                json_integer_value(json_object_get(native, "startTime")) > 0;
    sqlite3_stmt *source_row =
        valid
            ? prepare(db, "SELECT 1 FROM resource_ownership s WHERE s.resource_id=? "
                          "AND s.operation_id=? AND s.resource_type='workload' AND "
                          "s.ownership_state='held' AND s.lifecycle_stage='running' "
                          "AND EXISTS(SELECT 1 FROM resource_ownership r JOIN datastore_volumes v "
                          "ON v.volume_id=r.resource_id "
                          "WHERE r.operation_id=s.operation_id AND r.deployment_id=s.deployment_id "
                          "AND r.resource_type='datastore-volume' "
                          "AND r.ownership_state='held' AND v.allocated=1)")
            : NULL;
    valid = source_row != NULL;
    if (source_row) {
        bind(source_row, 1, source);
        bind(source_row, 2, target->operation_id);
        valid = sqlite3_step(source_row) == SQLITE_ROW && sqlite3_step(source_row) == SQLITE_DONE;
    }
    sqlite3_finalize(source_row);
    json_int_t attempt = 1;
    if (valid && control) {
        json_int_t old_purpose = json_integer_value(json_object_get(control, "purpose"));
        valid = !mutator && old_purpose != GSTD_EXEC_SCYLLA_KEYSPACE &&
                old_purpose != GSTD_EXEC_SCHEMA_MIGRATE && helper_control_valid(value, source) &&
                helper_text(control, "stage", "deleted") &&
                json_integer_value(json_object_get(control, "attempt")) < 128;
        if (valid)
            attempt = json_integer_value(json_object_get(control, "attempt")) + 1;
    } else if (valid && (json_object_get(value, "native") || json_object_get(value, "population")))
        valid = 0;
    if (valid && (mutator || after_mutation)) {
        char evidence_id[65];
        int n = snprintf(evidence_id, sizeof(evidence_id), "%s-%s",
                         mutator ? "run-cql" : "run-schema", target->operation_id);
        json_t *evidence = n > 0 && n < (int)sizeof(evidence_id)
                               ? owner_phase(db, target, evidence_id, "workload", 1)
                               : NULL;
        json_t *proof = json_object_get(evidence, "helperControl");
        json_int_t recorded = json_integer_value(json_object_get(proof, "purpose"));
        valid =
            evidence && helper_control_valid(evidence, source) &&
            helper_text(proof, "stage", "deleted") &&
            json_integer_value(json_object_get(proof, "result")) == 0 &&
            (mutator ? recorded == GSTD_EXEC_SCHEMA_INSPECT || recorded == GSTD_EXEC_SCHEMA_HISTORY
                     : recorded == GSTD_EXEC_SCHEMA_MIGRATE);
        json_decref(evidence);
    }
    if (valid && mutator)
        valid = !gstd_migration_transition(db, target->operation_id, GSTD_MIG_PREPARED,
                                           GSTD_MIG_DISPATCHED);
    json_t *next = valid ? json_pack("{s:i,s:s,s:I,s:s,s:b,s:n,s:n}", "purpose", purpose,
                                     "sourceDatastore", source, "attempt", attempt, "stage",
                                     "dispatch", "unsafe", mutator, "result", "termination")
                         : NULL;
    valid = next && !json_object_set(value, "helperControl", next);
    if (valid)
        json_object_del(value, "native");
    if (valid)
        json_object_del(value, "population");
    int result = valid ? write_owner(db, target, id, value, "allocated") : -1;
    json_decref(next);
    json_decref(value);
    json_decref(config);
    json_decref(datastore);
    json_decref(journal);
    return result;
}
int gstd_migration_service_stopping(sqlite3 *db, const gstd_deployment_target *target,
                                    const gstd_native_context *context) {
    if (!target || !target->migration || !context || context->control_owner != getpid() ||
        !context->boot_id || strlen(context->boot_id) != 36 || context->expected_pid <= 0 ||
        !context->expected_start_time || !context->component ||
        strcmp(context->component, "scylladb"))
        return -1;
    char source[65];
    int n = snprintf(source, sizeof(source), "run-scylladb-%s", target->operation_id);
    if (n < 1 || n >= (int)sizeof(source) || !context->backend_id ||
        strcmp(source, context->backend_id))
        return -1;
    json_t *value = owner_phase(db, target, source, "workload", 1);
    json_t *native = json_object_get(value, "native");
    int valid = value && json_is_object(native) && json_object_size(native) == 7 &&
                helper_text(native, "kind", "libcrun") && helper_text(native, "id", source) &&
                helper_text(native, "component", "scylladb") &&
                helper_text(native, "bootId", context->boot_id) &&
                json_integer_value(json_object_get(native, "generation")) ==
                    (json_int_t)target->generation &&
                json_integer_value(json_object_get(native, "pid")) == context->expected_pid &&
                json_integer_value(json_object_get(native, "startTime")) ==
                    (json_int_t)context->expected_start_time;
    /* Inspect all workload rows, not just the expected two helper names. An
     * unexpected native workload must not disappear beneath source teardown. */
    sqlite3_stmt *s = valid ? prepare(db, "SELECT resource_id,backend_identity,lifecycle_stage "
                                          "FROM resource_ownership WHERE operation_id=? AND "
                                          "deployment_id=? AND resource_type='workload' AND "
                                          "ownership_state='held'")
                            : NULL;
    valid = s != NULL;
    int source_seen = 0;
    if (s) {
        bind(s, 1, target->operation_id);
        bind(s, 2, target->deployment_id);
        int step;
        while ((step = sqlite3_step(s)) == SQLITE_ROW && valid) {
            const char *id = (const char *)sqlite3_column_text(s, 0);
            const char *stage = (const char *)sqlite3_column_text(s, 2);
            json_t *row =
                gstd_json_parse(sqlite3_column_text(s, 1), (size_t)sqlite3_column_bytes(s, 1));
            if (id && !strcmp(id, source)) {
                source_seen++;
                valid = stage && (!strcmp(stage, "running") || !strcmp(stage, "stopping"));
            } else {
                json_t *control = json_object_get(row, "helperControl");
                char expected_source[65];
                gstd_execution_purpose purpose =
                    (gstd_execution_purpose)json_integer_value(json_object_get(control, "purpose"));
                char cql[65], schema[65];
                int cql_length = snprintf(cql, sizeof(cql), "run-cql-%s", target->operation_id),
                    schema_length =
                        snprintf(schema, sizeof(schema), "run-schema-%s", target->operation_id);
                valid = cql_length > 0 && cql_length < (int)sizeof(cql) && schema_length > 0 &&
                        schema_length < (int)sizeof(schema) && row && id &&
                        (!strcmp(id, cql) || !strcmp(id, schema)) &&
                        json_equal(json_object_get(row, "executionManifest"), target->manifest) &&
                        (control ? helper_name(target, id, purpose, expected_source) &&
                                       helper_control_valid(row, source) &&
                                       helper_text(control, "stage", "deleted")
                                 : !json_object_get(row, "native") &&
                                       !json_object_get(row, "population"));
            }
            json_decref(row);
        }
        valid = valid && step == SQLITE_DONE && source_seen == 1;
    }
    sqlite3_finalize(s);
    int result = valid ? write_owner(db, target, source, value, "stopping") : -1;
    json_decref(value);
    return result;
}
int gstd_migration_service_deleted(sqlite3 *db, const gstd_deployment_target *target,
                                   const gstd_native_context *context,
                                   const gstd_native_population *population, int death_fd) {
    if (!context || !population || context->state_fd < 3 || !context->backend_id ||
        !identifier(context->backend_id) || gstd_native_watch_poll(death_fd) != 1 ||
        gstd_native_population_empty(population) != 1 || !population->device ||
        !population->inode || !population->events_inode)
        return -1;
    struct stat status;
    errno = 0;
    if (!fstatat(context->state_fd, context->backend_id, &status, AT_SYMLINK_NOFOLLOW) ||
        errno != ENOENT || gstd_migration_service_stopping(db, target, context))
        return -1;
    json_t *value = owner_phase(db, target, context->backend_id, "workload", 1);
    json_t *native = json_object_get(value, "native");
    char device[32], inode[32], events[32];
    snprintf(device, sizeof(device), "%llu", (unsigned long long)population->device);
    snprintf(inode, sizeof(inode), "%llu", (unsigned long long)population->inode);
    snprintf(events, sizeof(events), "%llu", (unsigned long long)population->events_inode);
    json_t *receipt =
        value ? json_pack("{s:s,s:s,s:I,s:O,s:s,s:s,s:s}", "kind", "migration-service-delete",
                          "operationId", target->operation_id, "generation",
                          (json_int_t)target->generation, "native", native, "cgroupDevice", device,
                          "cgroupInode", inode, "eventsInode", events)
              : NULL;
    json_t *prior = json_object_get(value, "termination");
    int result = receipt && (!prior || json_equal(prior, receipt)) &&
                         !json_object_set(value, "termination", receipt)
                     ? write_owner(db, target, context->backend_id, value, "retained")
                     : -1;
    json_decref(receipt);
    json_decref(value);
    return result;
}
static int helper_network_matches(sqlite3 *db, const gstd_deployment_target *target, const char *id,
                                  const char *source, const gstd_network_namespace *network) {
    if (!network || gstd_network_namespace_check(network))
        return 0;
    sqlite3_stmt *s = prepare(
        db, "SELECT a.boot_id,a.user_device,a.user_inode,a.network_device,a.network_inode FROM "
            "network_attachments a JOIN network_attachments b ON b.workload_id=? AND "
            "a.network_id=b.network_id AND a.operation_id=b.operation_id AND "
            "a.boot_id=b.boot_id AND a.user_device=b.user_device AND a.user_inode=b.user_inode "
            "AND a.network_device=b.network_device AND a.network_inode=b.network_inode JOIN "
            "resource_ownership n ON n.resource_id=a.network_id AND "
            "n.operation_id=a.operation_id AND n.ownership_state='held' JOIN resource_ownership w "
            "ON w.resource_id=b.workload_id AND w.operation_id=a.operation_id AND "
            "w.ownership_state='held' AND w.lifecycle_stage='running' AND "
            "json_valid(w.backend_identity) AND "
            "json_extract(w.backend_identity,'$.native.bootId')=a.boot_id WHERE "
            "a.workload_id=? AND a.operation_id=?");
    if (!s)
        return 0;
    bind(s, 1, source);
    bind(s, 2, id);
    bind(s, 3, target->operation_id);
    const char *boot = NULL;
    int valid = sqlite3_step(s) == SQLITE_ROW && (boot = (const char *)sqlite3_column_text(s, 0)) &&
                sqlite3_column_bytes(s, 0) == 36 && !strcmp(boot, network->boot_id);
    const uint64_t identities[] = {network->user_device, network->user_inode,
                                   network->network_device, network->network_inode};
    for (unsigned i = 0; valid && i < 4; i++) {
        char expected[32];
        snprintf(expected, sizeof(expected), "%llu", (unsigned long long)identities[i]);
        const char *stored = (const char *)sqlite3_column_text(s, (int)i + 1);
        valid = stored && sqlite3_column_bytes(s, (int)i + 1) == (int)strlen(expected) &&
                !strcmp(stored, expected);
    }
    valid = valid && sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return valid && !gstd_network_namespace_check(network);
}
static int helper_execution_matches(sqlite3 *db, const json_t *value,
                                    const gstd_deployment_target *target, const char *id,
                                    const gstd_helper *helper) {
    const gstd_execution_inputs *input = helper ? helper->context.registered_inputs : NULL;
    char source[65];
    struct stat root, bundle;
    json_t *execution = json_object_get(value, "execution"),
           *control = json_object_get(value, "helperControl"),
           *native = json_object_get(value, "native");
    return helper && input && helper_name(target, id, input->purpose, source) &&
           helper_control_valid(value, source) &&
           json_integer_value(json_object_get(control, "purpose")) == input->purpose &&
           helper->context.backend_id && !strcmp(helper->context.backend_id, id) &&
           helper->context.component &&
           !strcmp(helper->context.component,
                   input->purpose == GSTD_EXEC_SCHEMA_MIGRATE ? "api" : "scylladb") &&
           json_equal(input->manifest, target->manifest) &&
           helper_network_matches(db, target, id, source, input->network) &&
           (!native || (helper_text(native, "id", id) &&
                        helper_text(native, "component", helper->context.component) &&
                        json_integer_value(json_object_get(native, "generation")) ==
                            (json_int_t)target->generation)) &&
           !fstat(helper->context.rootfs_fd, &root) && !fstat(helper->context.bundle_fd, &bundle) &&
           identity_equals(execution, "rootfsDevice", root.st_dev) &&
           identity_equals(execution, "rootfsInode", root.st_ino) &&
           identity_equals(execution, "bundleDevice", bundle.st_dev) &&
           identity_equals(execution, "bundleInode", bundle.st_ino);
}
int gstd_deployment_helper_created(sqlite3 *db, const gstd_deployment_target *target,
                                   const char *id, const gstd_helper *helper) {
    if (!gstd_helper_identity_ready(helper))
        return -1;
    json_t *value = owner_phase(db, target, id, "workload", 1);
    int valid = value && helper_execution_matches(db, value, target, id, helper);
    json_t *control = json_object_get(value, "helperControl");
    json_t *native =
        valid ? json_pack("{s:s,s:s,s:s,s:I,s:I,s:s,s:I}", "kind", "libcrun", "id", id, "bootId",
                          helper->context.boot_id, "pid", (json_int_t)helper->context.expected_pid,
                          "startTime", (json_int_t)helper->context.expected_start_time, "component",
                          helper->context.component, "generation", (json_int_t)target->generation)
              : NULL;
    valid = native &&
            (helper_text(control, "stage", "dispatch") ||
             (helper_text(control, "stage", "created") &&
              json_equal(native, json_object_get(value, "native")))) &&
            !json_object_set(value, "native", native) &&
            !json_object_set_new(control, "stage", json_string("created"));
    json_t *population =
        valid ? gstd_native_population_identity(&helper->population, id, helper->context.boot_id)
              : NULL;
    json_t *prior = json_object_get(value, "population");
    valid = valid && population && (!prior || json_equal(prior, population)) &&
            !json_object_set(value, "population", population);
    int result = valid ? write_owner(db, target, id, value, "allocated") : -1;
    json_decref(population);
    json_decref(native);
    json_decref(value);
    return result;
}
int gstd_deployment_helper_finished(sqlite3 *db, const gstd_deployment_target *target,
                                    const char *id, const gstd_helper *helper) {
    if (!helper || helper->worker.worker || !helper->deleted || helper->may_exist ||
        (helper->outcome != 0 && helper->outcome != -1) ||
        (!helper->exit.exited && !helper->exit.signalled) ||
        helper->outcome !=
            (helper->exit.exited && !helper->exit.signalled && !helper->exit.code ? 0 : -1) ||
        gstd_native_watch_poll(helper->death_fd) != 1 ||
        gstd_native_population_empty(&helper->population) != 1)
        return -1;
    json_t *value = owner_phase(db, target, id, "workload", 1);
    int valid = value && helper_execution_matches(db, value, target, id, helper);
    json_t *control = json_object_get(value, "helperControl"),
           *native = json_object_get(value, "native");
    valid = valid && helper_text(native, "bootId", helper->context.boot_id) &&
            json_integer_value(json_object_get(native, "pid")) == helper->context.expected_pid &&
            json_integer_value(json_object_get(native, "startTime")) ==
                (json_int_t)helper->context.expected_start_time;
    char device[32], inode[32];
    snprintf(device, sizeof(device), "%llu", (unsigned long long)helper->population.device);
    snprintf(inode, sizeof(inode), "%llu", (unsigned long long)helper->population.inode);
    json_t *receipt =
        valid ? json_pack("{s:s,s:O,s:s,s:s,s:i,s:b}", "kind", "helper-native-delete", "native",
                          native, "cgroupDevice", device, "cgroupInode", inode, "exitCode",
                          helper->exit.code, "signalled", helper->exit.signalled)
              : NULL;
    valid = receipt &&
            (helper_text(control, "stage", "created") ||
             (helper_text(control, "stage", "deleted") &&
              json_equal(json_object_get(control, "termination"), receipt) &&
              json_integer_value(json_object_get(control, "result")) == helper->outcome)) &&
            !json_object_set(control, "termination", receipt) &&
            !json_object_set_new(control, "result", json_integer(helper->outcome)) &&
            !json_object_set_new(control, "unsafe", json_false()) &&
            !json_object_set_new(control, "stage", json_string("deleted"));
    int result = valid ? write_owner(db, target, id, value, "retained") : -1;
    json_decref(receipt);
    json_decref(value);
    return result;
}
static json_t *graph_value(const gstd_bootstrap_graph *graph) {
    if (!graph || graph->persistent > 1 || graph->fresh_datastore > 1 || graph->controller > 1 ||
        (graph->fresh_datastore && !graph->persistent) ||
        (graph->stage >= GSTD_BOOT_LAUNCH_DATASTORE && !graph->persistent) ||
        graph->stage < GSTD_BOOT_PREPARE || graph->stage >= GSTD_BOOT_STAGE_COUNT ||
        graph->outcome < -2 || graph->outcome > 0 ||
        strnlen(graph->controller_uid, sizeof(graph->controller_uid)) >=
            sizeof(graph->controller_uid))
        return NULL;
    json_t *record = json_pack("{s:I,s:i,s:b,s:i,s:s}", "generation", (json_int_t)graph->generation,
                               "stage", (int)graph->stage, "controller", graph->controller != 0,
                               "outcome", graph->outcome, "controllerUid", graph->controller_uid);
    if (record && graph->persistent &&
        (json_object_set_new(record, "persistent", json_true()) ||
         json_object_set_new(record, "freshDatastore", json_boolean(graph->fresh_datastore)))) {
        json_decref(record);
        return NULL;
    }
    return record;
}
int gstd_deployment_graph_recovery_read(const gstd_deployment_target *target, const json_t *value,
                                        gstd_bootstrap_graph *output) {
    if (!target || target->migration || !value || !output ||
        !json_equal(json_object_get(value, "executionManifest"), target->manifest))
        return -1;
    json_t *record = json_object_get(value, "graph");
    const char *uid = json_string_value(json_object_get(record, "controllerUid"));
    if (!uid ||
        json_string_length(json_object_get(record, "controllerUid")) >=
            sizeof(output->controller_uid) ||
        strlen(uid) != json_string_length(json_object_get(record, "controllerUid")))
        return -1;
    gstd_bootstrap_graph graph = {
        .generation = (uint64_t)json_integer_value(json_object_get(record, "generation")),
        .stage = (gstd_bootstrap_stage)json_integer_value(json_object_get(record, "stage")),
        .controller = json_is_true(json_object_get(record, "controller")),
        .persistent = json_is_true(json_object_get(record, "persistent")),
        .fresh_datastore = json_is_true(json_object_get(record, "freshDatastore")),
        .outcome = (int)json_integer_value(json_object_get(record, "outcome"))};
    if (graph.generation != target->generation || !graph.persistent)
        return -1;
    for (const unsigned char *p = (const unsigned char *)uid; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '-'))
            return -1;
    memcpy(graph.controller_uid, uid, strlen(uid) + 1);
    gstd_bootstrap_graph policy;
    if (gstd_bootstrap_graph_begin(target->manifest, "scylladb", target->generation, &policy) ||
        policy.controller != graph.controller || (!graph.controller && *uid))
        return -1;
    json_t *expected = graph_value(&graph);
    int valid = expected && json_equal(expected, record);
    json_decref(expected);
    if (valid)
        *output = graph;
    return valid ? 0 : -1;
}
static int retained_controller(sqlite3 *db, const gstd_deployment_target *target,
                               json_t *configuration, gstd_bootstrap_graph *graph) {
    if (!graph->persistent || graph->fresh_datastore || !graph->controller)
        return 0;
    sqlite3_stmt *s = prepare(
        db,
        "SELECT r.backend_identity,o.operation_id FROM operations o LEFT JOIN resource_ownership r "
        "ON r.resource_id='cfg-'||o.operation_id AND r.operation_id=o.operation_id AND "
        "r.deployment_id=o.deployment_id AND r.resource_type='configuration' AND "
        "r.ownership_state='released' AND r.lifecycle_stage='retained' WHERE o.deployment_id=? AND "
        "o.caller_uid=? AND "
        "o.method='deployment.start' AND o.state='succeeded' AND o.operation_id!=? AND "
        "json_extract(o.intent,'$.datastore')='scylladb' AND "
        "EXISTS(SELECT 1 FROM json_each(o.intent,'$.components') WHERE value='controller') "
        "ORDER BY o.rowid DESC LIMIT 1");
    if (!s)
        return -1;
    bind(s, 1, target->deployment_id);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
    bind(s, 3, target->operation_id);
    int step = sqlite3_step(s), valid = step == SQLITE_DONE;
    json_t *prior = NULL;
    char operation[65] = {0};
    if (step == SQLITE_ROW) {
        const unsigned char *bytes = sqlite3_column_text(s, 0);
        int size = sqlite3_column_bytes(s, 0);
        prior = bytes && size > 0 ? gstd_json_parse(bytes, (size_t)size) : NULL;
        json_t *old = json_object_get(prior, "graph");
        const char *uid = json_string_value(json_object_get(old, "controllerUid"));
        json_int_t generation = json_integer_value(json_object_get(prior, "generation"));
        valid = prior && !column(s, 1, operation, sizeof(operation)) &&
                helper_text(prior, "kind", "configuration") &&
                helper_text(prior, "storeId", target->store_id) &&
                helper_text(prior, "deploymentId", target->deployment_id) &&
                helper_text(prior, "operationId", operation) &&
                json_equal(json_object_get(prior, "executionManifest"), target->manifest) &&
                generation > 0 && generation < (json_int_t)target->generation &&
                json_integer_value(json_object_get(old, "generation")) == generation &&
                json_integer_value(json_object_get(old, "stage")) == GSTD_BOOT_COMPLETE &&
                json_is_integer(json_object_get(old, "outcome")) &&
                json_integer_value(json_object_get(old, "outcome")) == 0 &&
                json_is_true(json_object_get(prior, "credentialReservation")) &&
                json_is_true(json_object_get(old, "persistent")) &&
                json_is_true(json_object_get(old, "controller")) && uid &&
                json_string_length(json_object_get(old, "controllerUid")) > 0 &&
                json_string_length(json_object_get(old, "controllerUid")) <
                    sizeof(graph->controller_uid) &&
                strlen(uid) == json_string_length(json_object_get(old, "controllerUid"));
        for (const unsigned char *p = (const unsigned char *)uid; valid && p && *p; p++)
            valid = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                    (*p >= '0' && *p <= '9') || *p == '-';
        json_t *source =
            valid ? json_pack("{s:s,s:s,s:I,s:s}", "kind", "retained-datastore-enrollment",
                              "operationId", operation, "generation", generation, "uid", uid)
                  : NULL;
        valid = source && !json_object_set(configuration, "controllerIdentitySource", source);
        json_decref(source);
        if (valid)
            memcpy(graph->controller_uid, uid, strlen(uid) + 1);
    }
    json_decref(prior);
    sqlite3_finalize(s);
    return valid ? 0 : -1;
}
static int graph_begin(sqlite3 *db, const gstd_deployment_target *target, const char *id,
                       const char *volume, gstd_bootstrap_graph *output) {
    if (!output || !target || target->migration)
        return -1;
    json_t *value = owner(db, target, id, "configuration");
    if (!value || json_object_get(value, "graph") || !json_object_get(value, "scopeInode")) {
        json_decref(value);
        return -1;
    }
    gstd_bootstrap_graph graph;
    const char *datastore = json_string_value(json_object_get(target->manifest, "datastore"));
    int ok = !gstd_bootstrap_graph_begin(target->manifest, datastore ? datastore : "memdb",
                                         target->generation, &graph);
    if (ok && volume) {
        sqlite3_stmt *s = graph.persistent && identifier(volume)
                              ? prepare(db, "SELECT v.allocated,v.producer_operation_id FROM "
                                            "datastore_volumes v JOIN resource_ownership r ON "
                                            "r.resource_id=v.volume_id WHERE v.volume_id=? AND "
                                            "v.owner_uid=? AND v.store_id=? AND "
                                            "r.resource_type='datastore-volume' AND "
                                            "r.ownership_state='held' AND r.operation_id=? AND "
                                            "r.deployment_id=? AND r.lifecycle_stage='reserved'")
                              : NULL;
        ok = s != NULL;
        if (ok) {
            bind(s, 1, volume);
            sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
            bind(s, 3, target->store_id);
            bind(s, 4, target->operation_id);
            bind(s, 5, target->deployment_id);
            ok = sqlite3_step(s) == SQLITE_ROW;
            if (ok) {
                int allocated = sqlite3_column_int(s, 0);
                const char *producer = (const char *)sqlite3_column_text(s, 1);
                /* Freshness is captured before filesystem dispatch, never
                 * inferred from a later successful open or an empty directory. */
                ok = producer && (allocated == 1 ||
                                  (allocated == 0 && !strcmp(producer, target->operation_id)));
                graph.fresh_datastore = ok && allocated == 0;
                ok = ok && sqlite3_step(s) == SQLITE_DONE;
            }
        }
        sqlite3_finalize(s);
    }
    if (ok && retained_controller(db, target, value, &graph))
        ok = 0;
    json_t *record = ok ? graph_value(&graph) : NULL;
    ok = record && !json_object_set(value, "graph", record) &&
         !write_owner(db, target, id, value, "allocated");
    json_decref(record);
    json_decref(value);
    if (ok)
        *output = graph;
    return ok ? 0 : -1;
}
int gstd_deployment_graph_matches(sqlite3 *db, const gstd_deployment_target *target, const char *id,
                                  const gstd_bootstrap_graph *graph) {
    if (!target || target->migration || !graph || graph->generation != target->generation)
        return -1;
    json_t *value = owner(db, target, id, "configuration"), *expected = graph_value(graph);
    int matched = value && expected && json_equal(json_object_get(value, "graph"), expected);
    json_decref(value);
    json_decref(expected);
    return matched ? 0 : -1;
}
int gstd_deployment_graph_begin(sqlite3 *db, const gstd_deployment_target *target, const char *id,
                                gstd_bootstrap_graph *output) {
    return graph_begin(db, target, id, NULL, output);
}
int gstd_deployment_persistent_graph_begin(sqlite3 *db, const gstd_deployment_target *target,
                                           const char *id, const char *volume,
                                           gstd_bootstrap_graph *output) {
    return volume ? graph_begin(db, target, id, volume, output) : -1;
}
static int helper_result_proven(sqlite3 *db, const gstd_deployment_target *target,
                                gstd_bootstrap_stage stage, int result) {
    if (result < 0)
        return 1;
    if (!db || sqlite3_get_autocommit(db))
        return 0;
    sqlite3_stmt *active = prepare(db, "SELECT 1 FROM operations WHERE operation_id=? AND "
                                       "state IN ('accepted','running')");
    if (!active)
        return 0;
    bind(active, 1, target->operation_id);
    int running = sqlite3_step(active) == SQLITE_ROW;
    sqlite3_finalize(active);
    if (!running)
        return 0;
    gstd_execution_purpose purpose = stage == GSTD_BOOT_WAIT_DATASTORE ? GSTD_EXEC_SCYLLA_READY
                                     : stage == GSTD_BOOT_PREPARE_KEYSPACE
                                         ? GSTD_EXEC_SCYLLA_KEYSPACE
                                     : stage == GSTD_BOOT_MIGRATE_SCHEMA ? GSTD_EXEC_SCHEMA_MIGRATE
                                                                         : GSTD_EXEC_SERVICE;
    if (purpose == GSTD_EXEC_SERVICE)
        return 1;
    char id[65], source[65];
    int n = snprintf(id, sizeof(id), "%s-%s",
                     purpose == GSTD_EXEC_SCHEMA_MIGRATE ? "run-schema" : "run-cql",
                     target->operation_id);
    if (n < 1 || n >= (int)sizeof(id) || !helper_name(target, id, purpose, source))
        return 0;
    json_t *value = owner(db, target, id, "workload"),
           *control = json_object_get(value, "helperControl");
    int valid = value && helper_control_valid(value, source) &&
                helper_text(control, "stage", "deleted") &&
                json_integer_value(json_object_get(control, "purpose")) == purpose &&
                json_integer_value(json_object_get(control, "result")) == (result == 0 ? 0 : -1);
    json_decref(value);
    return valid;
}
int gstd_deployment_graph_step(sqlite3 *db, const gstd_deployment_target *target, const char *id,
                               gstd_bootstrap_graph *graph, gstd_bootstrap_stage stage, int result,
                               const char *uid) {
    if (!target || target->migration || !graph || graph->generation != target->generation)
        return -1;
    const char *datastore = json_string_value(json_object_get(target->manifest, "datastore"));
    int persistent = datastore && !strcmp(datastore, "scylladb");
    if (graph->persistent != (unsigned)persistent ||
        (persistent && !helper_result_proven(db, target, stage, result)))
        return -1;
    json_t *value = owner(db, target, id, "configuration"), *expected = graph_value(graph);
    int matched = value && expected && json_equal(json_object_get(value, "graph"), expected);
    json_decref(expected);
    gstd_bootstrap_graph next = *graph;
    int ok =
        matched && !gstd_bootstrap_graph_observe(&next, target->generation, stage, result, uid);
    json_t *record = ok ? graph_value(&next) : NULL;
    ok = record && !json_object_set(value, "graph", record) &&
         !write_owner(db, target, id, value, "allocated");
    json_decref(record);
    json_decref(value);
    if (ok)
        *graph = next;
    return ok ? 0 : -1;
}
int gstd_deployment_credential_reserve(sqlite3 *db, const gstd_deployment_target *target,
                                       const char *id, int *initial) {
    if (!initial)
        return -1;
    *initial = 0;
    if (!target || target->migration)
        return -1;
    json_t *value = owner(db, target, id, "configuration");
    json_t *graph = json_object_get(value, "graph");
    if (!value || !graph ||
        json_integer_value(json_object_get(graph, "stage")) != GSTD_BOOT_PREPARE ||
        json_integer_value(json_object_get(graph, "outcome")) != 0) {
        json_decref(value);
        return -1;
    }
    if (json_is_true(json_object_get(value, "credentialReservation"))) {
        json_decref(value);
        return 0;
    }
    sqlite3_stmt *s =
        prepare(db, "SELECT 1 FROM resource_ownership WHERE resource_type='configuration' AND "
                    "deployment_id=? AND json_valid(backend_identity) AND "
                    "json_extract(backend_identity,'$.credentialReservation')=1 LIMIT 1");
    int ok = s != NULL, prior = -1;
    if (ok) {
        bind(s, 1, target->deployment_id);
        int step = sqlite3_step(s);
        prior = step == SQLITE_ROW ? 1 : step == SQLITE_DONE ? 0 : -1;
    }
    sqlite3_finalize(s);
    ok = prior >= 0 && !json_object_set_new(value, "credentialReservation", json_true()) &&
         !write_owner(db, target, id, value, "allocated");
    json_decref(value);
    if (ok)
        *initial = !prior;
    return ok ? 0 : -1;
}

static json_t *oidc_configuration_owner(sqlite3 *db, const gstd_deployment_target *target,
                                        const char *id) {
    if (!target || target->migration || !identifier(target->operation_id) ||
        !identifier(target->deployment_id) || strnlen(target->store_id, 33) != 32 ||
        gstd_oidc_core_manifest_check(target->manifest) ||
        !json_object_get(target->manifest, "oidcProfileDigest") || !identifier(id))
        return NULL;
    char canonical[65];
    int n = snprintf(canonical, sizeof(canonical), "oidc-cfg-%s", target->operation_id);
    if (n < 1 || n >= (int)sizeof(canonical) || strcmp(canonical, id))
        return NULL;
    json_t *value = owner(db, target, id, "configuration");
    if (!value || !json_is_integer(json_object_get(value, "schemaVersion")) ||
        json_integer_value(json_object_get(value, "schemaVersion")) != 1 ||
        !json_equal(json_object_get(value, "profileDigest"),
                    json_object_get(target->manifest, "profileDigest")) ||
        !json_equal(json_object_get(value, "executionManifest"), target->manifest) ||
        !helper_text(value, "kind", "configuration") ||
        !helper_text(value, "storeId", target->store_id) ||
        !helper_text(value, "operationId", target->operation_id) ||
        !helper_text(value, "deploymentId", target->deployment_id) ||
        !json_is_integer(json_object_get(value, "generation")) ||
        json_integer_value(json_object_get(value, "generation")) !=
            (json_int_t)target->generation) {
        json_decref(value);
        return NULL;
    }
    sqlite3_stmt *s = prepare(
        db, "SELECT 1 FROM deployments d JOIN operations o ON o.operation_id=d.last_operation_id "
            "JOIN profiles p ON p.profile_id=d.profile_id AND p.profile_version=d.profile_version "
            "WHERE d.deployment_id=? AND d.generation=? AND o.operation_id=? AND o.caller_uid=? "
            "AND d.profile_id='core' AND d.profile_version='2' AND p.manifest_digest=? "
            "AND d.desired_state='running' AND d.recovery='normal' AND d.writer_ownership='held' "
            "AND o.state IN ('accepted','running') AND o.recovery='normal' "
            "AND json_extract(o.intent,'$.profileId')='core' AND "
            "json_extract(o.intent,'$.profileVersion')='2' "
            "AND EXISTS(SELECT 1 FROM json_each(o.intent,'$.components') WHERE value='oidc')");
    int eligible = s != NULL;
    if (s) {
        bind(s, 1, target->deployment_id);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)target->generation);
        bind(s, 3, target->operation_id);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)geteuid());
        bind(s, 5, json_string_value(json_object_get(target->manifest, "profileDigest")));
        eligible = sqlite3_step(s) == SQLITE_ROW && sqlite3_step(s) == SQLITE_DONE;
        sqlite3_finalize(s);
    }
    if (!eligible) {
        json_decref(value);
        return NULL;
    }
    return value;
}

static int oidc_credential_reference(const gstd_deployment_target *target, char reference[65]) {
    json_t *identity = json_pack("{s:s,s:s,s:s,s:s}", "installation", target->store_id, "issuer",
                                 "gitstore-local/reference-oidc/1", "account",
                                 target->deployment_id, "purpose", "oidc-provider-material");
    char hash[65];
    int valid = identity && !gstd_fingerprint(identity, hash);
    json_decref(identity);
    if (!valid) {
        return -1;
    }
    snprintf(reference, 65, "oidc-%.56s", hash);
    return 0;
}

int gstd_deployment_oidc_credential_reserve(sqlite3 *db, const gstd_deployment_target *target,
                                            const char *id, char reference[65], int *initial) {
    if (!reference || !initial)
        return -1;
    reference[0] = 0;
    *initial = 0;
    json_t *value = oidc_configuration_owner(db, target, id);
    if (!value)
        return -1;
    char expected[65];
    int valid = !oidc_credential_reference(target, expected);
    if (!valid) {
        json_decref(value);
        return -1;
    }
    if (json_object_get(value, "oidcCredentialReservation")) {
        valid = json_is_true(json_object_get(value, "oidcCredentialReservation")) &&
                helper_text(value, "oidcCredentialRef", expected);
        json_decref(value);
        if (valid)
            memcpy(reference, expected, strlen(expected) + 1);
        return valid ? 0 : -1;
    }
    if (json_object_get(value, "oidcCredentialRef")) {
        json_decref(value);
        return -1;
    }
    sqlite3_stmt *s = prepare(
        db, "SELECT backend_identity FROM resource_ownership WHERE resource_type='configuration' "
            "AND deployment_id=? AND backend_identity IS NOT NULL");
    int prior = 0;
    if (s) {
        bind(s, 1, target->deployment_id);
        int step;
        while ((step = sqlite3_step(s)) == SQLITE_ROW) {
            const unsigned char *raw = sqlite3_column_text(s, 0);
            int length = sqlite3_column_bytes(s, 0);
            json_t *old = raw && length > 0 ? gstd_json_parse(raw, (size_t)length) : NULL;
            if (!json_is_object(old))
                valid = 0;
            else if (json_object_get(old, "oidcCredentialReservation") ||
                     json_object_get(old, "oidcCredentialRef")) {
                valid = valid && json_is_true(json_object_get(old, "oidcCredentialReservation")) &&
                        helper_text(old, "oidcCredentialRef", expected);
                prior = 1;
            }
            json_decref(old);
            if (!valid)
                break;
        }
        valid = valid && step == SQLITE_DONE;
        sqlite3_finalize(s);
    } else
        valid = 0;
    valid = valid && !json_object_set_new(value, "oidcCredentialReservation", json_true()) &&
            !json_object_set_new(value, "oidcCredentialRef", json_string(expected)) &&
            !write_owner(db, target, id, value, "allocated");
    json_decref(value);
    if (valid) {
        memcpy(reference, expected, strlen(expected) + 1);
        *initial = !prior;
    }
    return valid ? 0 : -1;
}

int gstd_deployment_oidc_prepared_record(sqlite3 *db, const gstd_deployment_target *target,
                                         const char *id, int directory,
                                         const gstd_oidc_ports *ports,
                                         const gstd_filesystem_reply *reply) {
    if (directory < 3 || !ports || gstd_oidc_ports_check(ports) || !reply || reply->status ||
        reply->descriptor_count != 4)
        return -1;
    json_t *value = oidc_configuration_owner(db, target, id);
    struct stat scope;
    char reference[65];
    int valid = value && json_is_true(json_object_get(value, "oidcCredentialReservation")) &&
                json_object_get(value, "oidcMaterialSource") &&
                !oidc_credential_reference(target, reference) &&
                helper_text(value, "oidcCredentialRef", reference) && !fstat(directory, &scope) &&
                S_ISDIR(scope.st_mode) && scope.st_uid == geteuid() &&
                (scope.st_mode & 07777) == 0700 &&
                identity_equals(value, "dataDevice", (uint64_t)scope.st_dev) &&
                identity_equals(value, "dataInode", (uint64_t)scope.st_ino);
    json_t *files = valid ? json_array() : NULL;
    const char *roles[] = {"hydra-provider", "kratos-provider", "identity-schema",
                           "ui-environment"};
    struct stat identities[4];
    for (size_t i = 0; valid && i < 4; i++) {
        int fd = reply->descriptors[i];
        int flags = fd >= 3 ? fcntl(fd, F_GETFL) : -1;
        struct stat *info = &identities[i];
        valid = files && flags >= 0 && (flags & O_ACCMODE) == O_RDONLY && !fstat(fd, info) &&
                S_ISREG(info->st_mode) && info->st_uid == geteuid() &&
                (info->st_mode & 07777) == 0600 && info->st_nlink == 1 && info->st_size > 0 &&
                info->st_size <= 32768 && info->st_dev == scope.st_dev;
        for (size_t j = 0; valid && j < i; j++)
            valid = identities[j].st_ino != info->st_ino;
        if (valid) {
            char device[32], inode[32];
            snprintf(device, sizeof(device), "%llu", (unsigned long long)info->st_dev);
            snprintf(inode, sizeof(inode), "%llu", (unsigned long long)info->st_ino);
            json_t *file = json_pack("{s:s,s:s,s:s,s:I}", "role", roles[i], "device", device,
                                     "inode", inode, "size", (json_int_t)info->st_size);
            valid = file && !json_array_append(files, file);
            json_decref(file);
        }
    }
    json_t *prepared =
        valid ? json_pack("{s:i,s:[i,i,i,i,i],s:O}", "schemaVersion", 1, "ports", ports->issuer,
                          ports->bridge, ports->identity, ports->ui, ports->mail, "files", files)
              : NULL;
    const json_t *prior = json_object_get(value, "oidcPrepared");
    valid = prepared && (!prior || json_equal(prior, prepared));
    if (valid && !prior)
        valid = !json_object_set(value, "oidcPrepared", prepared) &&
                !write_owner(db, target, id, value, "allocated");
    json_decref(prepared);
    json_decref(files);
    json_decref(value);
    return valid ? 0 : -1;
}

static int oidc_decimal(const json_t *value, int nonzero) {
    const char *text = json_string_value(value);
    size_t length = json_string_length(value);
    if (!text || !length || length > 20 || (length > 1 && text[0] == '0'))
        return 0;
    uint64_t result = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned digit = (unsigned char)text[i] - (unsigned)'0';
        if (digit > 9 || result > (UINT64_MAX - digit) / 10)
            return 0;
        result = result * 10 + digit;
    }
    return !nonzero || result;
}
static int oidc_prepared_valid(const json_t *prepared) {
    const json_t *version = json_object_get(prepared, "schemaVersion"),
                 *tuple = json_object_get(prepared, "ports"),
                 *files = json_object_get(prepared, "files");
    if (!json_is_object(prepared) || json_object_size(prepared) != 3 || !json_is_integer(version) ||
        json_integer_value(version) != 1 || !json_is_array(tuple) || json_array_size(tuple) != 5 ||
        !json_is_array(files) || json_array_size(files) != 4)
        return 0;
    uint16_t ports[5];
    for (size_t i = 0; i < 5; i++) {
        const json_t *port = json_array_get(tuple, i);
        if (!json_is_integer(port) || json_integer_value(port) < 1024 ||
            json_integer_value(port) > 65535)
            return 0;
        ports[i] = (uint16_t)json_integer_value(port);
    }
    gstd_oidc_ports policy = {ports[0], ports[1], ports[2], ports[3], ports[4]};
    if (gstd_oidc_ports_check(&policy))
        return 0;
    const char *roles[] = {"hydra-provider", "kratos-provider", "identity-schema",
                           "ui-environment"};
    for (size_t i = 0; i < 4; i++) {
        const json_t *file = json_array_get(files, i), *size = json_object_get(file, "size");
        if (!json_is_object(file) || json_object_size(file) != 4 ||
            !helper_text(file, "role", roles[i]) ||
            !oidc_decimal(json_object_get(file, "device"), 0) ||
            !oidc_decimal(json_object_get(file, "inode"), 1) || !json_is_integer(size) ||
            json_integer_value(size) < 1 || json_integer_value(size) > 32768)
            return 0;
        for (size_t j = 0; j < i; j++) {
            const json_t *other = json_array_get(files, j);
            if (json_equal(json_object_get(file, "device"), json_object_get(other, "device")) &&
                json_equal(json_object_get(file, "inode"), json_object_get(other, "inode")))
                return 0;
        }
    }
    return 1;
}
int gstd_deployment_oidc_preparation_source(sqlite3 *db, const gstd_deployment_target *target,
                                            const char *id, char source_id[65], json_t **scope) {
    if (!source_id || !scope)
        return -1;
    source_id[0] = 0;
    *scope = NULL;
    json_t *value = oidc_configuration_owner(db, target, id), *selected = NULL;
    char reference[65], selected_id[65] = {0};
    int valid = value && !oidc_credential_reference(target, reference) &&
                json_is_true(json_object_get(value, "oidcCredentialReservation")) &&
                helper_text(value, "oidcCredentialRef", reference);
    sqlite3_stmt *s =
        valid
            ? prepare(
                  db,
                  "SELECT "
                  "r.resource_id,r.ownership_state,r.backend_identity,o.state,r.operation_id FROM "
                  "resource_ownership r JOIN operations o ON o.operation_id=r.operation_id WHERE "
                  "r.resource_type='configuration' AND r.deployment_id=? AND r.operation_id!=? "
                  "AND o.deployment_id=r.deployment_id AND r.backend_identity IS NOT NULL")
            : NULL;
    valid = s != NULL;
    json_int_t newest = 0;
    if (s) {
        bind(s, 1, target->deployment_id);
        bind(s, 2, target->operation_id);
        int step;
        while ((step = sqlite3_step(s)) == SQLITE_ROW) {
            const unsigned char *raw = sqlite3_column_text(s, 2);
            int length = sqlite3_column_bytes(s, 2);
            json_t *old = raw && length > 0 ? gstd_json_parse(raw, (size_t)length) : NULL;
            if (!json_is_object(old))
                valid = 0;
            else if (json_object_get(old, "oidcCredentialReservation") ||
                     json_object_get(old, "oidcCredentialRef")) {
                char old_id[65], ownership[16], state[16], canonical[65], operation_id[65];
                const json_t *generation = json_object_get(old, "generation");
                const char *operation = json_string_value(json_object_get(old, "operationId"));
                int n = operation ? snprintf(canonical, sizeof(canonical), "oidc-cfg-%s", operation)
                                  : -1;
                valid = !column(s, 0, old_id, sizeof(old_id)) &&
                        !column(s, 1, ownership, sizeof(ownership)) &&
                        !strcmp(ownership, "released") && !column(s, 3, state, sizeof(state)) &&
                        (!strcmp(state, "succeeded") || !strcmp(state, "failed") ||
                         !strcmp(state, "cancelled")) &&
                        n > 0 && n < (int)sizeof(canonical) && !strcmp(canonical, old_id) &&
                        !column(s, 4, operation_id, sizeof(operation_id)) && operation &&
                        !strcmp(operation_id, operation) &&
                        json_is_integer(json_object_get(old, "schemaVersion")) &&
                        json_integer_value(json_object_get(old, "schemaVersion")) == 1 &&
                        identifier(operation) &&
                        json_is_true(json_object_get(old, "oidcCredentialReservation")) &&
                        helper_text(old, "oidcCredentialRef", reference) &&
                        helper_text(old, "kind", "configuration") &&
                        helper_text(old, "storeId", target->store_id) &&
                        helper_text(old, "deploymentId", target->deployment_id) &&
                        json_is_integer(generation) && json_integer_value(generation) > 0 &&
                        json_integer_value(generation) < (json_int_t)target->generation &&
                        !gstd_oidc_core_manifest_check(json_object_get(old, "executionManifest")) &&
                        json_object_get(json_object_get(old, "executionManifest"),
                                        "oidcProfileDigest") &&
                        json_equal(json_object_get(old, "profileDigest"),
                                   json_object_get(target->manifest, "profileDigest")) &&
                        oidc_decimal(json_object_get(old, "dataDevice"), 0) &&
                        oidc_decimal(json_object_get(old, "dataInode"), 1) &&
                        oidc_prepared_valid(json_object_get(old, "oidcPrepared"));
                if (valid && json_integer_value(generation) == newest)
                    valid = 0;
                if (valid && json_integer_value(generation) > newest) {
                    newest = json_integer_value(generation);
                    strcpy(selected_id, old_id);
                    json_decref(selected);
                    selected = json_incref(old);
                }
            }
            json_decref(old);
            if (!valid)
                break;
        }
        valid = valid && step == SQLITE_DONE;
        sqlite3_finalize(s);
    }
    json_t *source = valid && selected
                         ? json_pack("{s:s,s:O,s:O,s:O,s:O,s:O}", "resourceId", selected_id,
                                     "operationId", json_object_get(selected, "operationId"),
                                     "generation", json_object_get(selected, "generation"),
                                     "dataDevice", json_object_get(selected, "dataDevice"),
                                     "dataInode", json_object_get(selected, "dataInode"),
                                     "prepared", json_object_get(selected, "oidcPrepared"))
                         : json_null();
    const json_t *prior = json_object_get(value, "oidcMaterialSource");
    valid = valid && source && (!prior || json_equal(prior, source));
    if (valid && !prior)
        valid = !json_object_set(value, "oidcMaterialSource", source) &&
                !write_owner(db, target, id, value, "allocated");
    if (valid && selected) {
        strcpy(source_id, selected_id);
        *scope = json_incref(selected);
    }
    int result = valid ? (selected ? 0 : 1) : -1;
    json_decref(source);
    json_decref(selected);
    json_decref(value);
    return result;
}

static json_t *oidc_original_configuration(sqlite3 *db, const gstd_deployment_target *target,
                                           const char *id) {
    if (!db || sqlite3_get_autocommit(db) || !target || target->migration || !identifier(id) ||
        !identifier(target->operation_id) || !identifier(target->deployment_id) ||
        strnlen(target->store_id, 33) != 32 || gstd_oidc_core_manifest_check(target->manifest) ||
        !json_object_get(target->manifest, "oidcProfileDigest"))
        return NULL;
    char canonical[65];
    int n = snprintf(canonical, sizeof(canonical), "oidc-cfg-%s", target->operation_id);
    if (n < 1 || n >= (int)sizeof(canonical) || strcmp(canonical, id))
        return NULL;
    sqlite3_stmt *s = prepare(
        db,
        "SELECT r.backend_identity FROM resource_ownership r JOIN operations o ON "
        "o.operation_id=r.operation_id AND o.deployment_id=r.deployment_id CROSS JOIN query_meta q "
        "WHERE r.resource_id=? AND r.resource_type='configuration' AND r.deployment_id=? AND "
        "r.operation_id=? AND r.ownership_state IN ('held','uncertain') AND "
        "o.caller_uid=? AND o.method='deployment.start' AND q.store_id=?");
    json_t *value = NULL;
    if (s) {
        bind(s, 1, id);
        bind(s, 2, target->deployment_id);
        bind(s, 3, target->operation_id);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)geteuid());
        bind(s, 5, target->store_id);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const unsigned char *raw = sqlite3_column_text(s, 0);
            int length = sqlite3_column_bytes(s, 0);
            value = raw && length > 0 ? gstd_json_parse(raw, (size_t)length) : NULL;
        }
        sqlite3_finalize(s);
    }
    if (!value || !json_is_integer(json_object_get(value, "schemaVersion")) ||
        json_integer_value(json_object_get(value, "schemaVersion")) != 1 ||
        !helper_text(value, "kind", "configuration") ||
        !helper_text(value, "storeId", target->store_id) ||
        !helper_text(value, "deploymentId", target->deployment_id) ||
        !helper_text(value, "operationId", target->operation_id) ||
        !json_is_integer(json_object_get(value, "generation")) ||
        json_integer_value(json_object_get(value, "generation")) !=
            (json_int_t)target->generation ||
        !json_equal(json_object_get(value, "profileDigest"),
                    json_object_get(target->manifest, "profileDigest")) ||
        !json_equal(json_object_get(value, "executionManifest"), target->manifest)) {
        json_decref(value);
        value = NULL;
    }
    return value;
}
static json_t *oidc_enrollment_binding(sqlite3 *db, const gstd_deployment_target *target,
                                       const json_t *configuration, int observation) {
    char reference[65], provider[65], volume[65], hash[65];
    int n = snprintf(provider, sizeof(provider), "run-oidc-hydra-%s", target->operation_id);
    if (n < 1 || n >= (int)sizeof(provider) || oidc_credential_reference(target, reference) ||
        !json_is_true(json_object_get(configuration, "oidcCredentialReservation")) ||
        !helper_text(configuration, "oidcCredentialRef", reference) ||
        !json_object_get(configuration, "oidcMaterialSource") ||
        !oidc_prepared_valid(json_object_get(configuration, "oidcPrepared")))
        return NULL;
    json_t *identity =
        json_pack("{s:s,s:s}", "deploymentId", target->deployment_id, "oidcProvider", "hydra");
    int valid = identity && !gstd_fingerprint(identity, hash);
    json_decref(identity);
    if (!valid)
        return NULL;
    snprintf(volume, sizeof(volume), "oidc-hydra-%.48s", hash);
    sqlite3_stmt *s = prepare(
        db,
        "SELECT v.producer_operation_id,v.data_device,v.data_inode FROM oidc_provider_volumes v "
        "JOIN resource_ownership r ON r.resource_id=v.volume_id WHERE v.volume_id=? AND "
        "v.role='oidc-hydra' AND v.owner_uid=? AND v.store_id=? AND v.compatibility=? AND "
        "v.allocated=1 AND r.resource_type='datastore-volume' AND r.deployment_id=? AND "
        "r.operation_id=? AND (r.ownership_state='held' OR (?=1 AND "
        "r.ownership_state='uncertain'))");
    json_t *binding = NULL;
    if (s) {
        bind(s, 1, volume);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
        bind(s, 3, target->store_id);
        bind(s, 4, gstd_oidc_volume_compatibility(GSTD_OIDC_VOLUME_HYDRA));
        bind(s, 5, target->deployment_id);
        bind(s, 6, target->operation_id);
        sqlite3_bind_int(s, 7, observation);
        char producer[65], device[32], inode[32];
        if (sqlite3_step(s) == SQLITE_ROW && !column(s, 0, producer, sizeof(producer)) &&
            !column(s, 1, device, sizeof(device)) && !column(s, 2, inode, sizeof(inode)))
            binding = json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:I,s:s,s:s}", "storeId",
                                target->store_id, "deploymentId", target->deployment_id,
                                "operationId", target->operation_id, "providerResourceId", provider,
                                "volumeProducerId", producer, "credentialRef", reference,
                                "generation", (json_int_t)target->generation, "volumeDevice",
                                device, "volumeInode", inode);
        sqlite3_finalize(s);
    }
    if (binding && !observation) {
        json_t *workload = owner(db, target, provider, "workload"),
               *native = json_object_get(workload, "native"),
               *population = json_object_get(workload, "population");
        s = prepare(db, "SELECT 1 FROM resource_ownership WHERE resource_id=? AND operation_id=? "
                        "AND resource_type='workload' AND ownership_state='held' AND "
                        "lifecycle_stage='running'");
        valid =
            s && workload && helper_text(native, "kind", "libcrun") &&
            helper_text(native, "id", provider) && helper_text(native, "component", "oidc-hydra") &&
            json_is_integer(json_object_get(native, "pid")) &&
            json_integer_value(json_object_get(native, "pid")) > 0 &&
            json_is_integer(json_object_get(native, "startTime")) &&
            json_integer_value(json_object_get(native, "startTime")) > 0 &&
            json_integer_value(json_object_get(native, "generation")) ==
                (json_int_t)target->generation &&
            json_is_object(population) && json_object_size(population) == 6 &&
            json_is_string(json_object_get(native, "bootId")) &&
            json_equal(json_object_get(native, "bootId"), json_object_get(population, "bootId"));
        if (s) {
            bind(s, 1, provider);
            bind(s, 2, target->operation_id);
            valid = valid && sqlite3_step(s) == SQLITE_ROW;
            sqlite3_finalize(s);
        }
        json_decref(workload);
        if (!valid) {
            json_decref(binding);
            binding = NULL;
        }
    }
    return binding;
}
int gstd_deployment_oidc_enrollment_begin(sqlite3 *db, const gstd_deployment_target *target,
                                          const char *id, const gstd_oidc_client *client,
                                          json_t **output) {
    if (!output)
        return -1;
    *output = NULL;
    json_t *value = oidc_configuration_owner(db, target, id),
           *binding = value ? oidc_enrollment_binding(db, target, value, 0) : NULL,
           *record = binding ? gstd_oidc_enrollment_prepare(binding, client) : NULL, *source = NULL;
    int valid = record != NULL;
    const json_t *existing = json_object_get(value, "oidcEnrollment");
    if (existing) {
        valid = valid && !gstd_oidc_enrollment_matches(existing, binding, client);
        json_decref(record);
        record = valid ? gstd_oidc_enrollment_recover(existing) : NULL;
    } else if (json_object_get(value, "oidcEnrollmentSource"))
        valid = 0;
    else if (valid) {
        sqlite3_stmt *s = prepare(
            db, "SELECT r.resource_id,r.backend_identity,r.ownership_state,o.state,r.operation_id "
                "FROM resource_ownership r JOIN operations o ON o.operation_id=r.operation_id AND "
                "o.deployment_id=r.deployment_id WHERE r.resource_type='configuration' AND "
                "r.deployment_id=? AND r.operation_id!=? AND r.backend_identity IS NOT NULL");
        valid = s != NULL;
        json_int_t newest = 0;
        if (s) {
            bind(s, 1, target->deployment_id);
            bind(s, 2, target->operation_id);
            int step;
            while ((step = sqlite3_step(s)) == SQLITE_ROW) {
                const unsigned char *raw = sqlite3_column_text(s, 1);
                int length = sqlite3_column_bytes(s, 1);
                json_t *old = raw && length > 0 ? gstd_json_parse(raw, (size_t)length) : NULL;
                const json_t *journal = json_object_get(old, "oidcEnrollment");
                if (!json_is_object(old) ||
                    (!journal && json_object_get(old, "oidcEnrollmentSource")))
                    valid = 0;
                if (valid && journal) {
                    char prior_id[65], ownership[16], state[16], operation[65];
                    const json_t *prior_binding = json_object_get(journal, "binding");
                    json_t *candidate = gstd_oidc_enrollment_resume(journal, binding, client);
                    valid = candidate && !column(s, 0, prior_id, sizeof(prior_id)) &&
                            !column(s, 2, ownership, sizeof(ownership)) &&
                            !strcmp(ownership, "released") && !column(s, 3, state, sizeof(state)) &&
                            strcmp(state, "accepted") && strcmp(state, "running") &&
                            !column(s, 4, operation, sizeof(operation)) &&
                            helper_text(old, "operationId", operation) &&
                            helper_text(prior_binding, "operationId", operation) &&
                            json_equal(json_object_get(old, "generation"),
                                       json_object_get(prior_binding, "generation"));
                    json_int_t generation =
                        json_integer_value(json_object_get(prior_binding, "generation"));
                    if (valid && generation == newest)
                        valid = 0;
                    if (valid && generation > newest) {
                        newest = generation;
                        json_decref(record);
                        record = candidate;
                        candidate = NULL;
                        json_decref(source);
                        source = json_pack("{s:s,s:O}", "resourceId", prior_id, "journal", journal);
                        valid = source != NULL;
                    }
                    json_decref(candidate);
                }
                json_decref(old);
                if (!valid)
                    break;
            }
            valid = valid && step == SQLITE_DONE;
            sqlite3_finalize(s);
        }
    }
    valid = valid && record && !json_object_set(value, "oidcEnrollment", record) &&
            (!source || !json_object_set(value, "oidcEnrollmentSource", source)) &&
            !write_owner(db, target, id, value, "allocated");
    if (valid)
        *output = json_incref(record);
    json_decref(source);
    json_decref(record);
    json_decref(binding);
    json_decref(value);
    return valid ? 0 : -1;
}
int gstd_deployment_oidc_enrollment_dispatch(sqlite3 *db, const gstd_deployment_target *target,
                                             const char *id, const gstd_oidc_client *client,
                                             const json_t *expected, json_t **output) {
    if (!output)
        return -1;
    *output = NULL;
    json_t *value = oidc_configuration_owner(db, target, id),
           *binding = value ? oidc_enrollment_binding(db, target, value, 0) : NULL,
           *record = binding && !gstd_oidc_enrollment_matches(expected, binding, client)
                         ? gstd_oidc_enrollment_dispatch(expected)
                         : NULL;
    int valid = record && json_equal(json_object_get(value, "oidcEnrollment"), expected) &&
                !json_object_set(value, "oidcEnrollment", record) &&
                !write_owner(db, target, id, value, "allocated");
    if (valid)
        *output = json_incref(record);
    json_decref(record);
    json_decref(binding);
    json_decref(value);
    return valid ? 0 : -1;
}
int gstd_deployment_oidc_enrollment_observe(sqlite3 *db, const gstd_deployment_target *target,
                                            const char *id, const gstd_oidc_client *client,
                                            const json_t *expected, gstd_oidc_http_status status,
                                            json_t **output) {
    if (!output)
        return -1;
    *output = NULL;
    json_t *value = oidc_original_configuration(db, target, id),
           *binding = value ? oidc_enrollment_binding(db, target, value, 1) : NULL,
           *record = binding && !gstd_oidc_enrollment_matches(expected, binding, client)
                         ? gstd_oidc_enrollment_observe(expected, status)
                         : NULL;
    const json_t *current = json_object_get(value, "oidcEnrollment");
    int valid = record && (json_equal(current, expected) || json_equal(current, record));
    if (valid && !json_equal(current, record)) {
        valid = !json_object_set(value, "oidcEnrollment", record);
        char *bytes = valid ? gstd_json_dump(value) : NULL;
        sqlite3_stmt *s =
            bytes && strlen(bytes) <= 32768
                ? prepare(db, "UPDATE resource_ownership SET "
                              "backend_identity=?,observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') "
                              "WHERE resource_id=? AND resource_type='configuration' AND "
                              "deployment_id=? AND operation_id=? "
                              "AND ownership_state IN ('held','uncertain')")
                : NULL;
        valid = s != NULL;
        if (s) {
            bind(s, 1, bytes);
            bind(s, 2, id);
            bind(s, 3, target->deployment_id);
            bind(s, 4, target->operation_id);
            valid = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
            sqlite3_finalize(s);
        }
        free(bytes);
    }
    if (valid)
        *output = json_incref(record);
    json_decref(record);
    json_decref(binding);
    json_decref(value);
    return valid ? 0 : -1;
}
