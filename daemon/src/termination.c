#define _POSIX_C_SOURCE 200809L
#include "termination.h"
#include "gitstored/protocol.h"
#include "native_exit.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int text(const json_t *object, const char *field, const char *expected) {
    json_t *v = json_object_get(object, field);
    return expected && json_is_string(v) && json_string_length(v) == strlen(expected) &&
           !memcmp(json_string_value(v), expected, strlen(expected));
}
static int positive(const json_t *object, const char *field) {
    const char *s = json_string_value(json_object_get(object, field));
    size_t length = json_string_length(json_object_get(object, field));
    if (!s || !length || length > 20 || s[0] == '0')
        return 0;
    for (size_t i = 0; i < length; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return 1;
}
static int owner(sqlite3 *db, const gstd_deployment_target *target, const char *operation,
                 uint64_t generation, int current) {
    sqlite3_stmt *s = NULL;
    int valid =
        sqlite3_prepare_v2(
            db,
            "SELECT 1 FROM operations o JOIN deployments d ON d.deployment_id=o.deployment_id "
            "WHERE o.operation_id=? AND o.caller_uid=? AND o.deployment_id=? AND "
            "o.state IN ('running','failed') AND (o.method='deployment.stop' OR "
            "(o.method='operation.cancel' AND o.target_operation_id=?)) AND d.generation>=? "
            "AND (?=0 OR (d.last_operation_id=o.operation_id AND d.generation=? AND "
            "o.state='running'))",
            -1, &s, NULL) == SQLITE_OK;
    if (valid) {
        sqlite3_bind_text(s, 1, operation, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
        sqlite3_bind_text(s, 3, target->deployment_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 4, target->operation_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 5, (sqlite3_int64)generation);
        sqlite3_bind_int(s, 6, current);
        sqlite3_bind_int64(s, 7, (sqlite3_int64)generation);
        valid = sqlite3_step(s) == SQLITE_ROW;
    }
    sqlite3_finalize(s);
    return valid ? 0 : -1;
}
int gstd_termination_read(sqlite3 *db, const gstd_deployment_target *target, const char *resource,
                          const json_t *identity) {
    if (!db || !target || !resource || !identity)
        return -1;
    json_t *receipt = json_object_get(identity, "termination"),
           *native = json_object_get(identity, "native");
    const char *operation = json_string_value(json_object_get(receipt, "operationId"));
    json_int_t generation = json_integer_value(json_object_get(receipt, "generation")),
               desired = json_integer_value(json_object_get(receipt, "desiredGeneration"));
    json_int_t pid = json_integer_value(json_object_get(native, "pid")),
               start = json_integer_value(json_object_get(native, "startTime"));
    const char *boot = json_string_value(json_object_get(native, "bootId"));
    if (!json_is_object(native) || json_object_size(native) != 7 ||
        !text(native, "kind", "libcrun") ||
        !(text(native, "component", "git") || text(native, "component", "api") ||
          text(native, "component", "controller") ||
          (text(native, "component", "scylladb") &&
           text(target->manifest, "datastore", "scylladb"))) ||
        !boot || json_string_length(json_object_get(native, "bootId")) != 36 ||
        strlen(boot) != 36 || !json_is_integer(json_object_get(native, "pid")) || pid <= 0 ||
        pid > INT_MAX || !json_is_integer(json_object_get(native, "startTime")) || start <= 0 ||
        start > GSTD_SAFE_INTEGER || !json_is_integer(json_object_get(native, "generation")) ||
        json_integer_value(json_object_get(native, "generation")) != (json_int_t)target->generation)
        return -1;
    if (!json_is_object(receipt) || json_object_size(receipt) != 8 ||
        !text(receipt, "kind", "libcrun-delete") || !text(native, "id", resource) ||
        !json_is_object(native) || !json_equal(native, json_object_get(receipt, "native")) ||
        !operation ||
        !gstd_identifier(operation, json_string_length(json_object_get(receipt, "operationId"))) ||
        !json_is_integer(json_object_get(receipt, "generation")) ||
        generation != (json_int_t)target->generation ||
        !json_is_integer(json_object_get(receipt, "desiredGeneration")) || desired <= generation ||
        desired > GSTD_SAFE_INTEGER || !positive(receipt, "cgroupDevice") ||
        !positive(receipt, "cgroupInode") || !positive(receipt, "eventsInode") ||
        owner(db, target, operation, (uint64_t)desired, 0))
        return -1;
    return 0;
}
int gstd_termination_record(gstd_deployment *d, gstd_deployment_workload *w, const char *operation,
                            uint64_t generation) {
    if (!d || !d->environment || !d->environment->database || !w || !operation || !*operation ||
        generation <= d->target.generation || generation > GSTD_SAFE_INTEGER ||
        sqlite3_get_autocommit(d->environment->database) || !w->launched || !w->exited ||
        (!(w->recovered_stopped && w->observed.stopped && w->death_fd == -1) &&
         gstd_native_watch_poll(w->death_fd) != 1) ||
        gstd_native_population_empty(&w->population) != 1)
        return -1;
    sqlite3 *db = d->environment->database;
    if (owner(db, &d->target, operation, generation, 1))
        return -1;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(
            db,
            "SELECT backend_identity FROM resource_ownership WHERE resource_id=? AND "
            "operation_id=? "
            "AND deployment_id=? AND resource_type='workload' AND lifecycle_stage='stopping' "
            "AND ownership_state='held'",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, w->resource, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, d->target.operation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, d->target.deployment_id, -1, SQLITE_TRANSIENT);
    json_t *value =
        sqlite3_step(s) == SQLITE_ROW
            ? gstd_json_parse(sqlite3_column_text(s, 0), (size_t)sqlite3_column_bytes(s, 0))
            : NULL;
    sqlite3_finalize(s);
    json_t *native = json_object_get(value, "native");
    char device[32], inode[32], events[32];
    snprintf(device, sizeof(device), "%llu", (unsigned long long)w->population.device);
    snprintf(inode, sizeof(inode), "%llu", (unsigned long long)w->population.inode);
    snprintf(events, sizeof(events), "%llu", (unsigned long long)w->population.events_inode);
    json_t *receipt =
        native ? json_pack("{s:s,s:s,s:I,s:I,s:O,s:s,s:s,s:s}", "kind", "libcrun-delete",
                           "operationId", operation, "generation", (json_int_t)d->target.generation,
                           "desiredGeneration", (json_int_t)generation, "native", native,
                           "cgroupDevice", device, "cgroupInode", inode, "eventsInode", events)
               : NULL;
    json_t *population = json_object_get(value, "population");
    int recovered_proof =
        !w->recovered_stopped ||
        (json_is_object(population) && json_object_size(population) == 6 &&
         text(population, "bootId", d->boot) && text(population, "cgroupDevice", device) &&
         text(population, "cgroupInode", inode) && text(population, "eventsInode", events));
    int valid = value && receipt && recovered_proof && text(native, "id", w->resource) &&
                text(native, "bootId", d->boot) &&
                json_integer_value(json_object_get(native, "pid")) == w->native.expected_pid &&
                json_integer_value(json_object_get(native, "startTime")) ==
                    (json_int_t)w->native.expected_start_time &&
                !json_object_get(value, "termination") &&
                !json_object_set(value, "termination", receipt) &&
                !gstd_termination_read(db, &d->target, w->resource, value);
    json_decref(receipt);
    char *bytes = valid ? gstd_json_dump(value) : NULL;
    json_decref(value);
    s = NULL;
    if (bytes && strlen(bytes) <= 32768 &&
        sqlite3_prepare_v2(
            db,
            "UPDATE resource_ownership SET backend_identity=?,lifecycle_stage='retained',"
            "observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE resource_id=? AND "
            "operation_id=? "
            "AND ownership_state='held' AND lifecycle_stage='stopping'",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, bytes, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, w->resource, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, d->target.operation_id, -1, SQLITE_TRANSIENT);
        valid = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    } else
        valid = 0;
    sqlite3_finalize(s);
    free(bytes);
    return valid ? 0 : -1;
}
