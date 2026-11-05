#define _POSIX_C_SOURCE 200809L
#include "network_records.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "migration_execution.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static sqlite3_stmt *prepare(sqlite3 *db, const char *text) {
    sqlite3_stmt *statement = NULL;
    return sqlite3_prepare_v2(db, text, -1, &statement, NULL) == SQLITE_OK ? statement : NULL;
}
static void bind(sqlite3_stmt *statement, int column, const char *text) {
    sqlite3_bind_text(statement, column, text, -1, SQLITE_TRANSIENT);
}
static int scope(const gstd_network_worker *worker) {
    struct stat info;
    return worker && worker->scope_fd >= 3 && !fstat(worker->scope_fd, &info) &&
           S_ISDIR(info.st_mode) && info.st_uid == geteuid() && (info.st_mode & 07777) == 0700 &&
           (uint64_t)info.st_dev == worker->scope_device &&
           (uint64_t)info.st_ino == worker->scope_inode;
}
static int operation_mode(sqlite3 *db, const char *operation, int observe) {
    if (!db || sqlite3_get_autocommit(db) || !operation)
        return 0;
    sqlite3_stmt *s = prepare(db, "SELECT method FROM operations WHERE operation_id=? AND "
                                  "caller_uid=? AND state IN ('accepted','running')");
    if (!s)
        return 0;
    bind(s, 1, operation);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
    int mode = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *method = (const char *)sqlite3_column_text(s, 0);
        mode = method && !strcmp(method, "deployment.start")  ? 1
               : method && !strcmp(method, "migration.start") ? 2
                                                              : 0;
    }
    sqlite3_finalize(s);
    if (mode == 2) {
        gstd_migration_execution execution = {0};
        int valid = observe ? !gstd_migration_execution_observe(db, operation, &execution)
                            : !gstd_migration_execution_read(db, operation, &execution);
        gstd_migration_execution_clear(&execution);
        if (!valid)
            mode = 0;
    }
    return mode;
}
static int worker_authorized(sqlite3 *db, const gstd_network_worker *worker,
                             const json_t *manifest) {
    int mode = worker ? operation_mode(db, worker->operation_id, 0) : 0;
    if (mode != 2)
        return mode;
    gstd_migration_execution execution = {0};
    char expected[65];
    int n = snprintf(expected, sizeof(expected), "net-%s", worker->operation_id);
    int valid =
        !gstd_migration_execution_read(db, worker->operation_id, &execution) && n > 0 &&
        n < (int)sizeof(expected) && !strcmp(worker->resource_id, expected) &&
        !strcmp(worker->store_id, execution.target.store_id) &&
        !strcmp(worker->profile_digest,
                json_string_value(json_object_get(execution.target.manifest, "profileDigest"))) &&
        (!manifest || json_equal(manifest, execution.target.manifest));
    gstd_migration_execution_clear(&execution);
    return valid ? mode : 0;
}
int gstd_network_attachment_matches(sqlite3 *db, const char *operation, const char *workload,
                                    const gstd_network_namespace *network) {
    if (!db || sqlite3_get_autocommit(db) || !operation || !workload ||
        !gstd_identifier(operation, strnlen(operation, 65)) ||
        !gstd_identifier(workload, strnlen(workload, 65)) || !network ||
        gstd_network_namespace_check(network))
        return -1;
    if (!operation_mode(db, operation, 1))
        return -1;
    sqlite3_stmt *s = prepare(
        db, "SELECT a.boot_id,a.user_device,a.user_inode,a.network_device,a.network_inode FROM "
            "network_attachments a JOIN resource_ownership w ON w.resource_id=a.workload_id "
            "AND w.operation_id=a.operation_id AND w.resource_type='workload' AND "
            "w.ownership_state='held' JOIN resource_ownership n ON n.resource_id=a.network_id "
            "AND n.operation_id=a.operation_id AND n.deployment_id=w.deployment_id AND "
            "n.resource_type='network' AND n.ownership_state='held' AND "
            "n.lifecycle_stage='running' JOIN operations o ON o.operation_id=a.operation_id "
            "AND o.deployment_id=w.deployment_id JOIN deployments d ON "
            "d.deployment_id=w.deployment_id WHERE "
            "a.operation_id=? AND a.workload_id=? AND o.caller_uid=? AND "
            "o.state IN ('accepted','running') AND d.recovery='normal' AND "
            "((o.method='deployment.start' AND d.last_operation_id=o.operation_id AND "
            "d.desired_state='running') OR "
            "(o.method='migration.start' AND d.desired_state='stopped')) "
            "AND json_valid(n.backend_identity) AND "
            "json_extract(n.backend_identity,'$.bootId')=a.boot_id");
    if (!s)
        return -1;
    bind(s, 1, operation);
    bind(s, 2, workload);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)geteuid());
    const char *boot = NULL;
    int valid = sqlite3_step(s) == SQLITE_ROW && (boot = (const char *)sqlite3_column_text(s, 0)) &&
                sqlite3_column_bytes(s, 0) == 36 && !strcmp(boot, network->boot_id);
    const uint64_t values[] = {network->user_device, network->user_inode, network->network_device,
                               network->network_inode};
    for (unsigned i = 0; valid && i < 4; i++) {
        char expected[32];
        snprintf(expected, sizeof(expected), "%llu", (unsigned long long)values[i]);
        const char *stored = (const char *)sqlite3_column_text(s, (int)i + 1);
        valid = stored && sqlite3_column_bytes(s, (int)i + 1) == (int)strlen(expected) &&
                !strcmp(stored, expected);
    }
    valid = valid && sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    return valid && !gstd_network_namespace_check(network) ? 0 : -1;
}
static int update(sqlite3 *db, const gstd_network_worker *worker, int running) {
    if (!db || sqlite3_get_autocommit(db) || !scope(worker) || !worker_authorized(db, worker, NULL))
        return -1;
    json_t *identity = gstd_network_worker_identity(worker);
    char *encoded = identity ? gstd_json_dump(identity) : NULL;
    json_decref(identity);
    if (!encoded)
        return -1;
    sqlite3_stmt *statement = prepare(
        db,
        "UPDATE resource_ownership SET backend_identity=?,lifecycle_stage=?,"
        "observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE resource_id=? AND "
        "resource_type='network' AND operation_id=? AND ownership_state='held' AND "
        "((?=0 AND lifecycle_stage IN ('reserved','allocated') AND "
        "(backend_identity IS NULL OR backend_identity=?)) OR "
        "(?=1 AND lifecycle_stage IN ('allocated','running') AND backend_identity=?)) AND "
        "EXISTS(SELECT 1 FROM operations o WHERE o.operation_id=resource_ownership.operation_id "
        "AND o.deployment_id=resource_ownership.deployment_id AND o.caller_uid=? AND "
        "o.method IN ('deployment.start','migration.start') AND o.state IN ('accepted','running')) "
        "AND "
        "EXISTS(SELECT 1 FROM query_meta WHERE store_id=?)");
    int result = -1;
    if (statement) {
        bind(statement, 1, encoded);
        bind(statement, 2, running ? "running" : "allocated");
        bind(statement, 3, worker->resource_id);
        bind(statement, 4, worker->operation_id);
        sqlite3_bind_int(statement, 5, running);
        bind(statement, 6, encoded);
        sqlite3_bind_int(statement, 7, running);
        bind(statement, 8, encoded);
        sqlite3_bind_int64(statement, 9, (sqlite3_int64)geteuid());
        bind(statement, 10, worker->store_id);
        result = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
        sqlite3_finalize(statement);
    }
    free(encoded);
    return result;
}
int gstd_network_record_bind(sqlite3 *db, const gstd_network_worker_request *request,
                             const gstd_network_worker *worker) {
    if (!request || !worker || !worker_authorized(db, worker, request->manifest) ||
        !request->store_id || !request->resource_id || !request->operation_id ||
        worker->channel < 3 || worker->pid_fd < 3 || worker->permitted || worker->received ||
        gstd_asset_manifest_check(request->manifest) ||
        strcmp(request->store_id, worker->store_id) ||
        strcmp(request->resource_id, worker->resource_id) ||
        strcmp(request->operation_id, worker->operation_id) ||
        strcmp(json_string_value(json_object_get(request->manifest, "profileDigest")),
               worker->profile_digest))
        return -1;
    unsigned roles = 0;
    if (gstd_network_request_roles(request, &roles) || roles != worker->roles)
        return -1;
    if (roles & 124) {
        json_t *wanted = gstd_network_oidc_ports_json(request->oidc_ports),
               *recorded = gstd_network_oidc_ports_json(&worker->oidc_ports);
        int matched = wanted && recorded && json_equal(wanted, recorded);
        json_decref(wanted);
        json_decref(recorded);
        if (!matched)
            return -1;
    }
    struct stat requested;
    if (request->scope_fd < 3 || fstat(request->scope_fd, &requested) ||
        (uint64_t)requested.st_dev != worker->scope_device ||
        (uint64_t)requested.st_ino != worker->scope_inode)
        return -1;
    return update(db, worker, 0);
}
int gstd_network_record_running(sqlite3 *db, const gstd_network_worker *worker,
                                const gstd_network_worker_observation *actual) {
    if (!worker || !actual || !worker->permitted || !worker->received ||
        actual->pid != worker->pid || actual->start != worker->start ||
        actual->network.user_fd < 3 || actual->network.network_fd < 3)
        return -1;
    return update(db, worker, 1);
}
int gstd_network_attachment_reserve(sqlite3 *db, const gstd_network_worker *worker,
                                    const gstd_network_worker_observation *actual,
                                    const char *workload) {
    if (!db || sqlite3_get_autocommit(db) || !workload ||
        !gstd_identifier(workload, strnlen(workload, 65)) || !scope(worker) ||
        !worker_authorized(db, worker, NULL) || !actual || !worker->permitted ||
        !worker->received || actual->pid != worker->pid || actual->start != worker->start ||
        strcmp(worker->boot_id, actual->network.boot_id) ||
        gstd_network_namespace_check(&actual->network))
        return -1;
    json_t *identity = gstd_network_worker_identity(worker);
    char *encoded = identity ? gstd_json_dump(identity) : NULL;
    json_decref(identity);
    if (!encoded)
        return -1;
    uint64_t numbers[4] = {actual->network.user_device, actual->network.user_inode,
                           actual->network.network_device, actual->network.network_inode};
    char decimals[4][32];
    for (int i = 0; i < 4; i++)
        snprintf(decimals[i], sizeof(decimals[i]), "%llu", (unsigned long long)numbers[i]);
    sqlite3_stmt *existing = prepare(
        db,
        "SELECT a.network_id,a.operation_id,a.boot_id,a.user_device,a.user_inode,"
        "a.network_device,a.network_inode FROM network_attachments a JOIN resource_ownership n "
        "ON a.network_id=n.resource_id JOIN resource_ownership w ON a.workload_id=w.resource_id "
        "AND w.deployment_id=n.deployment_id AND w.operation_id=a.operation_id "
        "JOIN operations o ON n.operation_id=o.operation_id WHERE "
        "w.resource_type='workload' AND w.ownership_state='held' AND "
        "a.workload_id=? AND n.resource_id=? AND n.backend_identity=? AND n.ownership_state='held' "
        "AND o.caller_uid=? AND o.method IN ('deployment.start','migration.start') AND o.state IN "
        "('accepted','running') "
        "AND EXISTS(SELECT 1 FROM query_meta WHERE store_id=?)");
    if (!existing) {
        free(encoded);
        return -1;
    }
    bind(existing, 1, workload);
    bind(existing, 2, worker->resource_id);
    bind(existing, 3, encoded);
    sqlite3_bind_int64(existing, 4, (sqlite3_int64)geteuid());
    bind(existing, 5, worker->store_id);
    int found = sqlite3_step(existing);
    if (found == SQLITE_ROW) {
        const char *wanted[7] = {worker->resource_id, worker->operation_id, worker->boot_id,
                                 decimals[0],         decimals[1],          decimals[2],
                                 decimals[3]};
        int equal = 1;
        for (int i = 0; i < 7; i++) {
            const unsigned char *text = sqlite3_column_text(existing, i);
            if (!text || sqlite3_column_bytes(existing, i) != (int)strlen(wanted[i]) ||
                memcmp(text, wanted[i], strlen(wanted[i])))
                equal = 0;
        }
        sqlite3_finalize(existing);
        free(encoded);
        return equal ? 0 : -1;
    }
    sqlite3_finalize(existing);
    if (found != SQLITE_DONE) {
        free(encoded);
        return -1;
    }
    sqlite3_stmt *statement = prepare(
        db, "INSERT INTO network_attachments SELECT ?,?,?,?, ?,?,?,? WHERE "
            "EXISTS(SELECT 1 FROM resource_ownership r JOIN operations o ON "
            "r.operation_id=o.operation_id "
            "WHERE r.resource_id=? AND r.backend_identity=? AND o.caller_uid=? "
            "AND r.resource_type='network' AND r.ownership_state='held' AND "
            "r.lifecycle_stage='running') AND "
            "EXISTS(SELECT 1 FROM query_meta WHERE store_id=?) AND "
            "EXISTS(SELECT 1 FROM resource_ownership w WHERE w.resource_id=? "
            "AND w.operation_id=? AND w.resource_type='workload' AND w.ownership_state='held')");
    int result = -1;
    if (statement) {
        bind(statement, 1, workload);
        bind(statement, 2, worker->resource_id);
        bind(statement, 3, worker->operation_id);
        bind(statement, 4, worker->boot_id);
        uint64_t values[4] = {actual->network.user_device, actual->network.user_inode,
                              actual->network.network_device, actual->network.network_inode};
        for (int i = 0; i < 4; i++) {
            char bytes[32];
            snprintf(bytes, sizeof(bytes), "%llu", (unsigned long long)values[i]);
            bind(statement, 5 + i, bytes);
        }
        bind(statement, 9, worker->resource_id);
        bind(statement, 10, encoded);
        sqlite3_bind_int64(statement, 11, (sqlite3_int64)geteuid());
        bind(statement, 12, worker->store_id);
        bind(statement, 13, workload);
        bind(statement, 14, worker->operation_id);
        result = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
        sqlite3_finalize(statement);
    }
    free(encoded);
    return result;
}
