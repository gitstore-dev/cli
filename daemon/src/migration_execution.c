#define _POSIX_C_SOURCE 200809L
#include "migration_execution.h"
#include "gitstored/assets.h"
#include "gitstored/profiles.h"
#include "gitstored/protocol.h"
#include "migration.h"
#include <string.h>
#include <unistd.h>
static int text(const json_t *v, const char *key, const char *expected) {
    json_t *field = json_object_get(v, key);
    return expected && json_is_string(field) && json_string_length(field) == strlen(expected) &&
           !memcmp(json_string_value(field), expected, strlen(expected));
}
void gstd_migration_execution_clear(gstd_migration_execution *execution) {
    if (execution) {
        gstd_deployment_target_clear(&execution->target);
        memset(execution, 0, sizeof(*execution));
    }
}
static int execution_read(sqlite3 *db, const char *operation, gstd_migration_execution *output,
                          int observation, const char *stop) {
    if (!db || sqlite3_get_autocommit(db) || !operation || !output ||
        !gstd_identifier(operation, strnlen(operation, 65)) ||
        (stop && !gstd_identifier(stop, strnlen(stop, 65))))
        return -1;
    json_t *control = gstd_migration_inspect(db, operation);
    const char *source = json_string_value(json_object_get(control, "sourceOperationId"));
    int active = text(control, "stage", "preflight") ||
                 (observation &&
                  (text(control, "stage", "prepared") || text(control, "stage", "dispatched") ||
                   text(control, "stage", "verified") || text(control, "stage", "cleanup")));
    if (stop && text(control, "stage", "uncertain"))
        active = 1;
    if (!control || !source || !active) {
        json_decref(control);
        return -1;
    }
    sqlite3_stmt *s = NULL;
    int valid =
        sqlite3_prepare_v2(
            db,
            "SELECT q.store_id,o.deployment_id,d.generation,p.server_revision,p.manifest_digest,"
            "p.platform,prior.intent,r.backend_identity FROM operations o JOIN deployments d "
            "ON d.deployment_id=o.deployment_id JOIN operations prior ON prior.operation_id=? "
            "AND prior.deployment_id=o.deployment_id AND prior.caller_uid=o.caller_uid "
            "AND prior.method='deployment.start' JOIN profiles p ON p.profile_id=d.profile_id "
            "AND p.profile_version=d.profile_version JOIN resource_ownership r ON "
            "r.resource_id='cfg-'||prior.operation_id AND r.operation_id=prior.operation_id "
            "AND r.deployment_id=d.deployment_id AND r.resource_type='configuration' "
            "AND r.ownership_state='released' AND r.lifecycle_stage='retained' CROSS JOIN "
            "query_meta q "
            "WHERE o.operation_id=? AND o.method='migration.start' AND o.caller_uid=? "
            "AND o.state=? AND o.recovery=? AND d.desired_state='stopped' "
            "AND d.recovery=? AND d.writer_ownership='released' "
            "AND d.profile_id='core' AND d.profile_version='1' AND EXISTS(SELECT 1 "
            "FROM resource_ownership m WHERE m.resource_id='migration-'||o.operation_id "
            "AND m.operation_id=o.operation_id AND (m.ownership_state='held' OR "
            "(? AND m.ownership_state='uncertain'))) "
            "AND NOT EXISTS(SELECT 1 FROM resource_ownership x WHERE "
            "x.deployment_id=d.deployment_id "
            "AND x.operation_id!=o.operation_id AND x.ownership_state!='released') "
            "AND prior.operation_id=(SELECT operation_id FROM operations WHERE "
            "deployment_id=d.deployment_id AND method='deployment.start' ORDER BY rowid DESC LIMIT "
            "1)",
            -1, &s, NULL) == SQLITE_OK;
    gstd_migration_execution execution = {0};
    json_t *intent = NULL, *configuration = NULL;
    if (valid) {
        sqlite3_bind_text(s, 1, source, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, operation, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)geteuid());
        sqlite3_bind_text(s, 4, stop ? "failed" : "running", -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 5, stop ? "uncertain" : "normal", -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 6, stop ? "uncertain" : "normal", -1, SQLITE_STATIC);
        sqlite3_bind_int(s, 7, stop != NULL);
        valid = sqlite3_step(s) == SQLITE_ROW;
    }
#if defined(__aarch64__) || defined(__arm64__)
    const char *architecture = "arm64", *platform = "linux-arm64";
#else
    const char *architecture = "amd64", *platform = "linux-amd64";
#endif
    if (valid) {
        const char *store = (const char *)sqlite3_column_text(s, 0),
                   *deployment = (const char *)sqlite3_column_text(s, 1),
                   *revision = (const char *)sqlite3_column_text(s, 3),
                   *digest = (const char *)sqlite3_column_text(s, 4),
                   *actual_platform = (const char *)sqlite3_column_text(s, 5);
        sqlite3_int64 current_generation = sqlite3_column_int64(s, 2);
        sqlite3_int64 generation =
            stop ? json_integer_value(json_object_get(control, "generation")) : current_generation;
        intent = gstd_json_parse(sqlite3_column_text(s, 6), (size_t)sqlite3_column_bytes(s, 6));
        configuration =
            gstd_json_parse(sqlite3_column_text(s, 7), (size_t)sqlite3_column_bytes(s, 7));
        valid = store && sqlite3_column_bytes(s, 0) == 32 && strlen(store) == 32 && deployment &&
                gstd_identifier(deployment, (size_t)sqlite3_column_bytes(s, 1)) && generation > 0 &&
                generation <= GSTD_SAFE_INTEGER && (!stop || current_generation > generation) &&
                generation == json_integer_value(json_object_get(control, "generation")) &&
                intent && configuration && text(intent, "profileId", "core") &&
                text(intent, "profileVersion", "1") && text(intent, "datastore", "scylladb");
        if (valid) {
            gstd_deployment_target *target = &execution.target;
            target->components = gstd_profile_components(json_object_get(intent, "components"));
            target->manifest = target->components
                                   ? gstd_asset_manifest_for_datastore(target->components,
                                                                       architecture, "scylladb")
                                   : NULL;
            valid =
                target->manifest && actual_platform && !strcmp(actual_platform, platform) &&
                text(target->manifest, "serverRevision", revision) &&
                text(target->manifest, "profileDigest", digest) &&
                json_equal(json_object_get(configuration, "executionManifest"), target->manifest) &&
                json_equal(json_object_get(target->manifest, "migrationTarget"),
                           json_object_get(control, "targetVersion")) &&
                json_equal(json_object_get(target->manifest, "migrationTargetDigest"),
                           json_object_get(control, "targetDigest")) &&
                text(configuration, "storeId", store) &&
                text(configuration, "operationId", source) &&
                text(configuration, "deploymentId", deployment) &&
                text(configuration, "kind", "configuration") &&
                json_is_integer(json_object_get(configuration, "schemaVersion")) &&
                json_integer_value(json_object_get(configuration, "schemaVersion")) == 1 &&
                json_equal(json_object_get(configuration, "profileDigest"),
                           json_object_get(target->manifest, "profileDigest")) &&
                json_is_integer(json_object_get(configuration, "generation")) &&
                json_integer_value(json_object_get(configuration, "generation")) > 0 &&
                json_integer_value(json_object_get(configuration, "generation")) < generation;
            if (valid) {
                memcpy(target->store_id, store, 33);
                memcpy(target->deployment_id, deployment, strlen(deployment) + 1);
                memcpy(target->operation_id, operation, strlen(operation) + 1);
                target->generation = (uint64_t)generation;
                target->migration = 1;
                memcpy(execution.source_operation, source, strlen(source) + 1);
            }
        }
        valid = valid && sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    s = NULL;
    if (valid && stop) {
        valid = sqlite3_prepare_v2(
                    db,
                    "SELECT r.backend_identity FROM operations o JOIN deployments d ON "
                    "d.deployment_id=o.deployment_id JOIN resource_ownership r ON "
                    "r.deployment_id=d.deployment_id WHERE o.operation_id=? AND o.caller_uid=? "
                    "AND o.method='deployment.stop' AND o.state='running' AND o.recovery='normal' "
                    "AND d.last_operation_id=o.operation_id AND d.generation>? AND "
                    "d.deployment_id=? AND r.resource_id='cfg-'||? AND r.operation_id=? AND "
                    "r.resource_type='configuration' AND r.ownership_state IN ('held','uncertain')",
                    -1, &s, NULL) == SQLITE_OK;
        if (valid) {
            sqlite3_bind_text(s, 1, stop, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
            sqlite3_bind_int64(s, 3, (sqlite3_int64)execution.target.generation);
            sqlite3_bind_text(s, 4, execution.target.deployment_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 5, operation, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 6, operation, -1, SQLITE_TRANSIENT);
            json_t *own =
                sqlite3_step(s) == SQLITE_ROW
                    ? gstd_json_parse(sqlite3_column_text(s, 0), (size_t)sqlite3_column_bytes(s, 0))
                    : NULL;
            valid =
                own && text(own, "kind", "configuration") && text(own, "operationId", operation) &&
                text(own, "deploymentId", execution.target.deployment_id) &&
                text(own, "storeId", execution.target.store_id) &&
                json_is_integer(json_object_get(own, "generation")) &&
                json_integer_value(json_object_get(own, "generation")) ==
                    (json_int_t)execution.target.generation &&
                json_equal(json_object_get(own, "executionManifest"), execution.target.manifest) &&
                !json_object_get(own, "graph") && sqlite3_step(s) == SQLITE_DONE;
            json_decref(own);
        }
        sqlite3_finalize(s);
    }
    json_decref(configuration);
    json_decref(intent);
    json_decref(control);
    if (!valid) {
        gstd_migration_execution_clear(&execution);
        return -1;
    }
    *output = execution;
    return 0;
}

int gstd_migration_execution_read(sqlite3 *db, const char *operation,
                                  gstd_migration_execution *output) {
    return execution_read(db, operation, output, 0, NULL);
}
int gstd_migration_execution_observe(sqlite3 *db, const char *operation,
                                     gstd_migration_execution *output) {
    return execution_read(db, operation, output, 1, NULL);
}

int gstd_migration_execution_recovery_read(sqlite3 *db, const char *operation, const char *stop,
                                           gstd_migration_execution *output) {
    return stop ? execution_read(db, operation, output, 1, stop) : -1;
}
