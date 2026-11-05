#define _POSIX_C_SOURCE 200809L
#include "native_recovery.h"
#include "gitstored/protocol.h"
#include "native_exit.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int text(const json_t *v, const char *key, const char *expected) {
    json_t *field = json_object_get(v, key);
    return expected && json_is_string(field) && json_string_length(field) == strlen(expected) &&
           !memcmp(json_string_value(field), expected, strlen(expected));
}
int gstd_native_recovery_read(const json_t *record, const char *stage, const char *resource,
                              const char *component, const char *boot, uint64_t generation,
                              gstd_native_observation *output) {
    if (!json_is_object(record) || !stage || !resource || !component || !boot || !generation ||
        generation > GSTD_SAFE_INTEGER || !output)
        return -1;
    *output = (gstd_native_observation){0};
    json_t *native = json_object_get(record, "native");
    if (!native)
        return !strcmp(stage, "reserved") && !json_object_get(record, "population")
                   ? GSTD_RECOVERY_UNDISPATCHED
                   : -1;
    json_t *pid = json_object_get(native, "pid"), *start = json_object_get(native, "startTime"),
           *epoch = json_object_get(native, "generation");
    if (!json_is_object(native) || json_object_size(native) != 7 ||
        !text(native, "kind", "libcrun") || !text(native, "id", resource) ||
        !text(native, "component", component) || !text(native, "bootId", boot) ||
        !json_is_integer(epoch) || json_integer_value(epoch) != (json_int_t)generation ||
        !json_is_integer(pid) || json_integer_value(pid) < 0 || json_integer_value(pid) > INT_MAX ||
        !json_is_integer(start) || json_integer_value(start) < 0 ||
        json_integer_value(start) > GSTD_SAFE_INTEGER)
        return -1;
    output->pid = (pid_t)json_integer_value(pid);
    output->start_time = (uint64_t)json_integer_value(start);
    if (!output->pid && !output->start_time)
        return !strcmp(stage, "allocated") && !json_object_get(record, "population")
                   ? GSTD_RECOVERY_NATIVE_DISCOVER
                   : -1;
    if (!output->pid || !output->start_time ||
        (strcmp(stage, "allocated") && strcmp(stage, "running") && strcmp(stage, "stopping") &&
         strcmp(stage, "retained")))
        return -1;
    return GSTD_RECOVERY_NATIVE_KNOWN;
}
int gstd_native_recovery_record(gstd_deployment *d, gstd_deployment_workload *w,
                                const json_t *expected) {
    if (!d || !d->environment || !d->environment->database || !d->recovered || !w || !w->launched ||
        w->exited || w->native.expected_pid <= 0 || !w->native.expected_start_time ||
        w->observed.pid != w->native.expected_pid ||
        w->observed.start_time != w->native.expected_start_time ||
        (!w->observed.running && !w->observed.created) ||
        sqlite3_get_autocommit(d->environment->database) ||
        gstd_native_watch_poll(w->death_fd) != 0 ||
        gstd_native_population_matches(&w->population, w->resource, w->native.expected_pid,
                                       w->native.expected_start_time, d->boot))
        return -1;
    gstd_native_observation original;
    if (gstd_native_recovery_read(expected, "allocated", w->resource, w->name, d->boot,
                                  d->target.generation, &original) != GSTD_RECOVERY_NATIVE_DISCOVER)
        return -1;
    sqlite3 *db = d->environment->database;
    sqlite3_stmt *s = NULL;
    int valid =
        sqlite3_prepare_v2(
            db,
            "SELECT r.backend_identity FROM resource_ownership r JOIN operations o "
            "ON o.operation_id=r.operation_id AND o.deployment_id=r.deployment_id "
            "JOIN deployments d ON d.deployment_id=r.deployment_id WHERE r.resource_id=? "
            "AND r.operation_id=? AND r.deployment_id=? AND r.resource_type='workload' "
            "AND r.lifecycle_stage='allocated' AND r.ownership_state IN ('held','uncertain') "
            "AND o.method='deployment.start' AND o.caller_uid=? AND d.generation=? "
            "AND d.last_operation_id=?",
            -1, &s, NULL) == SQLITE_OK;
    json_t *record = NULL;
    if (valid) {
        sqlite3_bind_text(s, 1, w->resource, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, d->target.operation_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, d->target.deployment_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)geteuid());
        sqlite3_bind_int64(s, 5, (sqlite3_int64)d->recovery_generation);
        sqlite3_bind_text(s, 6, d->recovery_operation, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const unsigned char *bytes = sqlite3_column_text(s, 0);
            int size = sqlite3_column_bytes(s, 0);
            if (bytes && size > 0 && size <= 32768)
                record = gstd_json_parse(bytes, (size_t)size);
        }
    }
    sqlite3_finalize(s);
    valid = record && json_equal(record, expected);
    json_t *native = json_object_get(record, "native");
    valid = valid && !json_object_set_new(native, "pid", json_integer(w->native.expected_pid)) &&
            !json_object_set_new(native, "startTime",
                                 json_integer((json_int_t)w->native.expected_start_time));
    json_t *population =
        valid ? gstd_native_population_identity(&w->population, w->resource, d->boot) : NULL;
    json_t *prior_population = json_object_get(record, "population");
    valid = valid && population &&
            (!prior_population || json_equal(prior_population, population)) &&
            !json_object_set(record, "population", population);
    json_decref(population);
    char *bytes = valid ? gstd_json_dump(record) : NULL;
    json_decref(record);
    s = NULL;
    valid = bytes && sqlite3_prepare_v2(
                         db,
                         "UPDATE resource_ownership SET backend_identity=?,lifecycle_stage=?,"
                         "observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE resource_id=? "
                         "AND operation_id=? AND deployment_id=? AND lifecycle_stage='allocated' "
                         "AND ownership_state IN ('held','uncertain')",
                         -1, &s, NULL) == SQLITE_OK;
    if (valid) {
        sqlite3_bind_text(s, 1, bytes, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, w->observed.running ? "running" : "allocated", -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 3, w->resource, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 4, d->target.operation_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 5, d->target.deployment_id, -1, SQLITE_TRANSIENT);
        valid = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    }
    sqlite3_finalize(s);
    free(bytes);
    return valid ? 0 : -1;
}
