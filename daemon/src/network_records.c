#define _POSIX_C_SOURCE 200809L
#include "network_records.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
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
static int update(sqlite3 *db, const gstd_network_worker *worker, int running) {
    if (!db || sqlite3_get_autocommit(db) || !scope(worker))
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
        "o.method='deployment.start' AND o.state IN ('accepted','running')) AND "
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
    if (!request || !worker || !request->store_id || !request->resource_id ||
        !request->operation_id || worker->channel < 3 || worker->pid_fd < 3 || worker->permitted ||
        worker->received || gstd_asset_manifest_check(request->manifest) ||
        strcmp(request->store_id, worker->store_id) ||
        strcmp(request->resource_id, worker->resource_id) ||
        strcmp(request->operation_id, worker->operation_id) ||
        strcmp(json_string_value(json_object_get(request->manifest, "profileDigest")),
               worker->profile_digest))
        return -1;
    unsigned roles = 0;
    json_t *images = json_object_get(request->manifest, "images");
    for (size_t i = 0; i < json_array_size(images); i++) {
        const char *component =
            json_string_value(json_object_get(json_array_get(images, i), "component"));
        if (!strcmp(component, "api"))
            roles |= 1;
        if (!strcmp(component, "git"))
            roles |= 2;
    }
    if (roles != worker->roles)
        return -1;
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
        !gstd_identifier(workload, strnlen(workload, 65)) || !scope(worker) || !actual ||
        !worker->permitted || !worker->received || actual->pid != worker->pid ||
        actual->start != worker->start || strcmp(worker->boot_id, actual->network.boot_id) ||
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
        "AND o.caller_uid=? AND o.method='deployment.start' AND o.state IN ('accepted','running') "
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
    sqlite3_stmt *statement =
        prepare(db, "INSERT INTO network_attachments SELECT ?,?,?,?, ?,?,?,? WHERE "
                    "EXISTS(SELECT 1 FROM resource_ownership r JOIN operations o ON "
                    "r.operation_id=o.operation_id "
                    "WHERE r.resource_id=? AND r.backend_identity=? AND o.caller_uid=?) AND "
                    "EXISTS(SELECT 1 FROM query_meta WHERE store_id=?)");
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
        result = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
        sqlite3_finalize(statement);
    }
    free(encoded);
    return result;
}
