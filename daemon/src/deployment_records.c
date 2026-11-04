#define _POSIX_C_SOURCE 200809L
#include "deployment_records.h"
#include "gitstored/assets.h"
#include "gitstored/profiles.h"
#include "gitstored/protocol.h"
#include <fcntl.h>
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
        !datastore || strcmp(datastore, "memdb"))
        goto failed;
    target.components = gstd_profile_components(json_object_get(intent, "components"));
#if defined(__aarch64__) || defined(__arm64__)
    const char *architecture = "arm64";
#else
    const char *architecture = "amd64";
#endif
    target.manifest =
        target.components ? gstd_asset_manifest(target.components, architecture) : NULL;
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
static int current(sqlite3 *db, const gstd_deployment_target *target) {
    if (!db || sqlite3_get_autocommit(db) || !target || !identifier(target->operation_id) ||
        !identifier(target->deployment_id) || !target->generation ||
        gstd_asset_manifest_check(target->manifest))
        return 0;
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
static json_t *owner(sqlite3 *db, const gstd_deployment_target *target, const char *id,
                     const char *kind) {
    if (!current(db, target) || !identifier(id))
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
    return value;
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
                !fstat(scope->descriptors[0], &wrapper) && !fstat(scope->descriptors[1], &data) &&
                !fstat(scope->descriptors[2], &bundle) && !fstat(rootfs, &root);
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
static json_t *graph_value(const gstd_bootstrap_graph *graph) {
    return json_pack("{s:I,s:i,s:b,s:i,s:s}", "generation", (json_int_t)graph->generation, "stage",
                     (int)graph->stage, "controller", graph->controller != 0, "outcome",
                     graph->outcome, "controllerUid", graph->controller_uid);
}
int gstd_deployment_graph_begin(sqlite3 *db, const gstd_deployment_target *target, const char *id,
                                gstd_bootstrap_graph *output) {
    if (!output)
        return -1;
    json_t *value = owner(db, target, id, "configuration");
    if (!value || json_object_get(value, "graph") || !json_object_get(value, "scopeInode")) {
        json_decref(value);
        return -1;
    }
    gstd_bootstrap_graph graph;
    int ok = !gstd_bootstrap_graph_begin(target->manifest, "memdb", target->generation, &graph);
    json_t *record = ok ? graph_value(&graph) : NULL;
    ok = record && !json_object_set(value, "graph", record) &&
         !write_owner(db, target, id, value, "allocated");
    json_decref(record);
    json_decref(value);
    if (ok)
        *output = graph;
    return ok ? 0 : -1;
}
int gstd_deployment_graph_step(sqlite3 *db, const gstd_deployment_target *target, const char *id,
                               gstd_bootstrap_graph *graph, gstd_bootstrap_stage stage, int result,
                               const char *uid) {
    if (!target || !graph || graph->generation != target->generation)
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
