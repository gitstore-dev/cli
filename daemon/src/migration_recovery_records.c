#define _POSIX_C_SOURCE 200809L
#include "migration_recovery_records.h"
#include "gitstored/asset_records.h"
#include "migration.h"
#include "native_exit.h"
#include "native_recovery.h"
#include "termination.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    return sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK ? s : NULL;
}
static void bind(sqlite3_stmt *s, int index, const char *value) {
    sqlite3_bind_text(s, index, value, -1, SQLITE_TRANSIENT);
}
static int text(const json_t *value, const char *field, const char *expected) {
    json_t *v = json_object_get(value, field);
    return expected && json_is_string(v) && json_string_length(v) == strlen(expected) &&
           !memcmp(json_string_value(v), expected, strlen(expected));
}
static int current(sqlite3 *db, const gstd_deployment_target *target, const char *stop,
                   uint64_t *generation) {
    gstd_migration_execution execution = {0};
    int valid =
        target &&
        !gstd_migration_execution_recovery_read(db, target->operation_id, stop, &execution) &&
        target->generation == execution.target.generation &&
        !strcmp(target->deployment_id, execution.target.deployment_id) &&
        !strcmp(target->store_id, execution.target.store_id) &&
        json_equal(target->manifest, execution.target.manifest);
    gstd_migration_execution_clear(&execution);
    sqlite3_stmt *s = valid ? prepare(db, "SELECT generation FROM deployments WHERE "
                                          "deployment_id=? AND last_operation_id=?")
                            : NULL;
    valid = s != NULL;
    if (s) {
        bind(s, 1, target->deployment_id);
        bind(s, 2, stop);
        valid = sqlite3_step(s) == SQLITE_ROW &&
                sqlite3_column_int64(s, 0) > (sqlite3_int64)target->generation &&
                sqlite3_column_int64(s, 0) <= GSTD_SAFE_INTEGER;
        if (valid && generation)
            *generation = (uint64_t)sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    return valid ? 0 : -1;
}
int gstd_migration_recovery_claim(sqlite3 *db, const char *migration, const char *stop,
                                  gstd_migration_execution *output, uint64_t *generation) {
    if (!db || sqlite3_get_autocommit(db) || !migration || !stop || !output || !generation ||
        !gstd_identifier(migration, strnlen(migration, 65)) ||
        !gstd_identifier(stop, strnlen(stop, 65)))
        return -1;
    json_t *control = gstd_migration_inspect(db, migration);
    json_int_t source_generation = json_integer_value(json_object_get(control, "generation"));
    sqlite3_stmt *s =
        control ? prepare(db, "SELECT d.generation,d.last_operation_id FROM operations m "
                              "JOIN deployments d ON d.deployment_id=m.deployment_id JOIN "
                              "operations o ON o.deployment_id=d.deployment_id "
                              "WHERE m.operation_id=? AND m.method='migration.start' AND "
                              "m.state='failed' AND m.recovery='uncertain' "
                              "AND m.caller_uid=? AND o.operation_id=? AND "
                              "o.caller_uid=m.caller_uid AND o.method='deployment.stop' "
                              "AND o.state='running' AND o.recovery='normal' AND "
                              "d.desired_state='stopped' AND d.recovery='uncertain' "
                              "AND NOT EXISTS(SELECT 1 FROM operations x WHERE "
                              "x.deployment_id=d.deployment_id AND "
                              "x.state IN ('accepted','running') AND x.method!='operation.cancel' "
                              "AND x.operation_id!=o.operation_id)")
                : NULL;
    int valid = s != NULL;
    uint64_t selected = 0;
    int claimed = 0;
    if (s) {
        bind(s, 1, migration);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
        bind(s, 3, stop);
        valid = sqlite3_step(s) == SQLITE_ROW && source_generation > 0 &&
                source_generation <= GSTD_SAFE_INTEGER &&
                sqlite3_column_int64(s, 0) >= source_generation &&
                sqlite3_column_int64(s, 0) <= GSTD_SAFE_INTEGER;
        if (valid) {
            selected = (uint64_t)sqlite3_column_int64(s, 0);
            const char *last = (const char *)sqlite3_column_text(s, 1);
            claimed = last && !strcmp(last, stop);
            valid = !claimed || selected > (uint64_t)source_generation;
        }
    }
    sqlite3_finalize(s);
    json_decref(control);
    if (!valid)
        return -1;
    if (!claimed) {
        if (selected >= GSTD_SAFE_INTEGER)
            return -1;
        s = prepare(db, "UPDATE deployments SET "
                        "generation=generation+1,last_operation_id=?,readiness='pending',"
                        "observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE "
                        "deployment_id=(SELECT deployment_id "
                        "FROM operations WHERE operation_id=?) AND generation=? AND "
                        "recovery='uncertain' AND desired_state='stopped'");
        if (!s)
            return -1;
        bind(s, 1, stop);
        bind(s, 2, migration);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)selected);
        valid = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(s);
        if (!valid)
            return -1;
        selected++;
    }
    if (gstd_migration_execution_recovery_read(db, migration, stop, output))
        return -1;
    *generation = selected;
    return 0;
}
static json_t *owner(sqlite3 *db, const gstd_deployment_target *target, const char *id) {
    sqlite3_stmt *s = prepare(
        db,
        "SELECT backend_identity FROM resource_ownership WHERE resource_id=? AND operation_id=? "
        "AND deployment_id=? AND resource_type='workload' AND ownership_state='held'");
    json_t *value = NULL;
    if (s) {
        bind(s, 1, id);
        bind(s, 2, target->operation_id);
        bind(s, 3, target->deployment_id);
        if (sqlite3_step(s) == SQLITE_ROW)
            value = gstd_json_parse(sqlite3_column_text(s, 0), (size_t)sqlite3_column_bytes(s, 0));
    }
    sqlite3_finalize(s);
    if (value && !json_equal(json_object_get(value, "executionManifest"), target->manifest)) {
        json_decref(value);
        value = NULL;
    }
    return value;
}
static int write_owner(sqlite3 *db, const gstd_deployment_target *target, const char *id,
                       json_t *value, const char *stage) {
    char *bytes = gstd_json_dump(value);
    sqlite3_stmt *s =
        bytes && strlen(bytes) <= 32768
            ? prepare(db, "UPDATE resource_ownership SET backend_identity=?,"
                          "lifecycle_stage=?,observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') "
                          "WHERE resource_id=? AND operation_id=? AND ownership_state='held'")
            : NULL;
    int valid = s != NULL;
    if (s) {
        bind(s, 1, bytes);
        bind(s, 2, stage);
        bind(s, 3, id);
        bind(s, 4, target->operation_id);
        valid = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    }
    sqlite3_finalize(s);
    free(bytes);
    return valid ? 0 : -1;
}
static int native_matches(const json_t *native, const gstd_deployment_target *target,
                          const gstd_native_context *context) {
    return json_is_object(native) && json_object_size(native) == 7 &&
           text(native, "kind", "libcrun") && text(native, "id", context->backend_id) &&
           text(native, "component", context->component) &&
           text(native, "bootId", context->boot_id) &&
           json_integer_value(json_object_get(native, "generation")) ==
               (json_int_t)target->generation &&
           json_integer_value(json_object_get(native, "pid")) == context->expected_pid &&
           json_integer_value(json_object_get(native, "startTime")) ==
               (json_int_t)context->expected_start_time;
}
static int helper_role(const gstd_deployment_target *target, const char *id,
                       const json_t *control) {
    char expected[80], source[80];
    json_int_t purpose = json_integer_value(json_object_get(control, "purpose"));
    if (!json_is_integer(json_object_get(control, "purpose")) ||
        (purpose != GSTD_EXEC_SCYLLA_READY && purpose != GSTD_EXEC_SCHEMA_MIGRATE &&
         purpose != GSTD_EXEC_SCHEMA_INSPECT && purpose != GSTD_EXEC_SCHEMA_HISTORY))
        return 0;
    snprintf(expected, sizeof(expected), "%s-%s",
             purpose == GSTD_EXEC_SCHEMA_MIGRATE ? "run-schema" : "run-cql", target->operation_id);
    snprintf(source, sizeof(source), "run-scylladb-%s", target->operation_id);
    return id && !strcmp(expected, id) && json_object_size(control) == 7 &&
           json_is_boolean(json_object_get(control, "unsafe")) &&
           (text(control, "stage", "deleted") || json_is_true(json_object_get(control, "unsafe")) ==
                                                     (purpose == GSTD_EXEC_SCHEMA_MIGRATE)) &&
           text(control, "sourceDatastore", source) &&
           json_is_integer(json_object_get(control, "attempt")) &&
           json_integer_value(json_object_get(control, "attempt")) > 0 &&
           json_integer_value(json_object_get(control, "attempt")) <= 128;
}
int gstd_migration_recovery_helper_active_read(const gstd_deployment_target *target, const char *id,
                                               const json_t *value, const char *boot,
                                               gstd_native_observation *output) {
    json_t *control = json_object_get(value, "helperControl");
    if (!target || !target->migration || !output || !helper_role(target, id, control) ||
        !json_equal(json_object_get(value, "executionManifest"), target->manifest) ||
        !json_is_null(json_object_get(control, "result")) ||
        !json_is_null(json_object_get(control, "termination")))
        return -1;
    if (text(control, "stage", "dispatch") && !json_object_get(value, "native") &&
        !json_object_get(value, "population")) {
        *output = (gstd_native_observation){0};
        return GSTD_RECOVERY_NATIVE_DISCOVER;
    }
    if (!text(control, "stage", "created"))
        return -1;
    return gstd_native_recovery_read(value, "allocated", id,
                                     json_integer_value(json_object_get(control, "purpose")) ==
                                             GSTD_EXEC_SCHEMA_MIGRATE
                                         ? "api"
                                         : "scylladb",
                                     boot, target->generation, output);
}
int gstd_migration_recovery_native_discovered(sqlite3 *db, const gstd_deployment_target *target,
                                              const char *stop, const gstd_deployment_workload *w,
                                              const json_t *expected) {
    if (!w || !w->launched || w->exited || (!w->observed.created && !w->observed.running) ||
        current(db, target, stop, NULL) || gstd_native_watch_poll(w->death_fd) != 0 ||
        gstd_native_population_matches(&w->population, w->resource, w->native.expected_pid,
                                       w->native.expected_start_time, w->native.boot_id))
        return -1;
    json_t *value = owner(db, target, w->resource);
    gstd_native_observation original = {0};
    char source[80];
    snprintf(source, sizeof(source), "run-scylladb-%s", target->operation_id);
    int helper = strcmp(source, w->resource) != 0;
    int classification =
        helper ? gstd_migration_recovery_helper_active_read(target, w->resource, value,
                                                            w->native.boot_id, &original)
               : gstd_native_recovery_read(value, "allocated", w->resource, "scylladb",
                                           w->native.boot_id, target->generation, &original);
    int valid = value && json_equal(value, expected) &&
                ((classification == GSTD_RECOVERY_NATIVE_DISCOVER && w->observed.created &&
                  !w->observed.running) ||
                 (classification == GSTD_RECOVERY_NATIVE_KNOWN &&
                  native_matches(json_object_get(value, "native"), target, &w->native)));
    json_t *native =
        valid ? json_pack("{s:s,s:s,s:s,s:I,s:I,s:s,s:I}", "kind", "libcrun", "id", w->resource,
                          "bootId", w->native.boot_id, "pid", (json_int_t)w->native.expected_pid,
                          "startTime", (json_int_t)w->native.expected_start_time, "component",
                          w->name, "generation", (json_int_t)target->generation)
              : NULL;
    json_t *population =
        native ? gstd_native_population_identity(&w->population, w->resource, w->native.boot_id)
               : NULL;
    json_t *prior_population = json_object_get(value, "population");
    valid = population && (!prior_population || json_equal(prior_population, population)) &&
            !json_object_set(value, "native", native) &&
            !json_object_set(value, "population", population);
    if (valid && helper)
        valid = !json_object_set_new(json_object_get(value, "helperControl"), "stage",
                                     json_string("created"));
    int result = valid ? write_owner(db, target, w->resource, value,
                                     helper || !w->observed.running ? "allocated" : "running")
                       : -1;
    json_decref(native);
    json_decref(population);
    json_decref(value);
    return result;
}
int gstd_migration_recovery_helper_deleted(sqlite3 *db, const gstd_deployment_target *target,
                                           const char *stop, const gstd_deployment_workload *w) {
    uint64_t generation;
    if (!w || !w->launched || !w->exited || current(db, target, stop, &generation) ||
        (!(w->recovered_stopped && w->observed.stopped && w->death_fd == -1) &&
         gstd_native_watch_poll(w->death_fd) != 1) ||
        gstd_native_population_empty(&w->population) != 1)
        return -1;
    struct stat info;
    errno = 0;
    if (!fstatat(w->native.state_fd, w->resource, &info, AT_SYMLINK_NOFOLLOW) || errno != ENOENT)
        return -1;
    json_t *value = owner(db, target, w->resource), *native = json_object_get(value, "native"),
           *control = json_object_get(value, "helperControl");
    int valid = value && helper_role(target, w->resource, control) &&
                text(control, "stage", "created") &&
                json_is_null(json_object_get(control, "result")) &&
                json_is_null(json_object_get(control, "termination")) &&
                native_matches(native, target, &w->native);
    char device[32], inode[32], events[32];
    snprintf(device, sizeof(device), "%llu", (unsigned long long)w->population.device);
    snprintf(inode, sizeof(inode), "%llu", (unsigned long long)w->population.inode);
    snprintf(events, sizeof(events), "%llu", (unsigned long long)w->population.events_inode);
    json_t *population = json_object_get(value, "population");
    valid = valid && text(population, "bootId", w->native.boot_id) &&
            text(population, "cgroupDevice", device) && text(population, "cgroupInode", inode) &&
            text(population, "eventsInode", events);
    json_t *receipt =
        valid ? json_pack("{s:s,s:s,s:I,s:O,s:s,s:s,s:s,s:b}", "kind", "helper-native-delete",
                          "stopOperationId", stop, "controlGeneration", (json_int_t)generation,
                          "native", native, "cgroupDevice", device, "cgroupInode", inode,
                          "eventsInode", events, "exitStatusKnown", 0)
              : NULL;
    json_t *prior = json_object_get(value, "reconciliation");
    int result = receipt && (!prior || json_equal(prior, receipt)) &&
                         !json_object_set(value, "reconciliation", receipt)
                     ? write_owner(db, target, w->resource, value, "retained")
                     : -1;
    json_decref(receipt);
    json_decref(value);
    return result;
}
static int positive(const json_t *value, const char *field) {
    json_t *v = json_object_get(value, field);
    const char *p = json_string_value(v);
    size_t n = json_string_length(v);
    if (!p || !n || n > 20 || p[0] == '0')
        return 0;
    for (size_t i = 0; i < n; i++)
        if (p[i] < '0' || p[i] > '9')
            return 0;
    return 1;
}
static int helper_reconciled(const json_t *value, const char *stop, uint64_t generation) {
    json_t *receipt = json_object_get(value, "reconciliation"),
           *population = json_object_get(value, "population");
    return json_is_object(receipt) && json_object_size(receipt) == 8 &&
           text(receipt, "kind", "helper-native-delete") &&
           text(receipt, "stopOperationId", stop) &&
           json_is_integer(json_object_get(receipt, "controlGeneration")) &&
           positive(receipt, "cgroupDevice") && positive(receipt, "cgroupInode") &&
           positive(receipt, "eventsInode") &&
           json_integer_value(json_object_get(receipt, "controlGeneration")) ==
               (json_int_t)generation &&
           json_is_false(json_object_get(receipt, "exitStatusKnown")) &&
           json_equal(json_object_get(receipt, "native"), json_object_get(value, "native")) &&
           json_is_object(population) && json_object_size(population) == 6 &&
           json_equal(json_object_get(receipt, "cgroupDevice"),
                      json_object_get(population, "cgroupDevice")) &&
           json_equal(json_object_get(receipt, "cgroupInode"),
                      json_object_get(population, "cgroupInode")) &&
           json_equal(json_object_get(receipt, "eventsInode"),
                      json_object_get(population, "eventsInode"));
}
static int deleted_absent(const char *id, const json_t *native, const char *boot, int native_fd) {
    if (!id || !gstd_identifier(id, strnlen(id, 65)) || native_fd < 3 || !boot ||
        strlen(boot) != 36 || !text(native, "bootId", boot))
        return -1;
    json_int_t pid = json_integer_value(json_object_get(native, "pid")),
               birth = json_integer_value(json_object_get(native, "startTime"));
    if (!json_is_integer(json_object_get(native, "pid")) || pid <= 0 || pid > INT_MAX ||
        !json_is_integer(json_object_get(native, "startTime")) || birth <= 0 ||
        birth > GSTD_SAFE_INTEGER)
        return -1;
    char current_boot[37];
    struct stat st;
    if (gstd_native_boot_id(current_boot) || strcmp(current_boot, boot))
        return -1;
    errno = 0;
    if (!fstatat(native_fd, id, &st, AT_SYMLINK_NOFOLLOW) || errno != ENOENT)
        return -1;
    uint64_t actual;
    errno = 0;
    if (!gstd_native_process_start((pid_t)pid, &actual)) {
        if (actual != (uint64_t)birth)
            return 0;
        int watch = -1;
        if (gstd_native_watch_open((pid_t)pid, (uint64_t)birth, boot, &watch))
            return -1;
        int dead = gstd_native_watch_poll(watch);
        close(watch);
        return dead == 1 ? 0 : -1;
    }
    return errno == ENOENT || errno == ESRCH ? 0 : -1;
}
static int original_source_deleted(const gstd_deployment_target *target, const char *id,
                                   const json_t *value) {
    json_t *receipt = json_object_get(value, "termination"),
           *native = json_object_get(value, "native");
    return json_is_object(receipt) && json_object_size(receipt) == 7 &&
           text(receipt, "kind", "migration-service-delete") &&
           text(receipt, "operationId", target->operation_id) &&
           json_is_integer(json_object_get(receipt, "generation")) &&
           json_integer_value(json_object_get(receipt, "generation")) ==
               (json_int_t)target->generation &&
           json_equal(json_object_get(receipt, "native"), native) && json_is_object(native) &&
           json_object_size(native) == 7 && text(native, "id", id) &&
           text(native, "kind", "libcrun") && text(native, "component", "scylladb") &&
           json_integer_value(json_object_get(native, "generation")) ==
               (json_int_t)target->generation &&
           positive(receipt, "cgroupDevice") && positive(receipt, "cgroupInode") &&
           positive(receipt, "eventsInode");
}
int gstd_migration_recovery_service_receipt_read(sqlite3 *db, const gstd_deployment_target *target,
                                                 const char *id, const json_t *value,
                                                 const char *boot, int native_fd) {
    char source[80];
    if (!target)
        return -1;
    snprintf(source, sizeof(source), "run-scylladb-%s", target->operation_id);
    if (!id || strcmp(source, id) ||
        !json_equal(json_object_get(value, "executionManifest"), target->manifest) ||
        (gstd_termination_read(db, target, id, value) &&
         !original_source_deleted(target, id, value)))
        return -1;
    return deleted_absent(id, json_object_get(value, "native"), boot, native_fd);
}
int gstd_migration_recovery_helper_receipt_read(sqlite3 *db, const gstd_deployment_target *target,
                                                const char *id, const json_t *value,
                                                const char *boot, int native_fd) {
    if (!db || !target || !target->migration || native_fd < 3 || !boot || strlen(boot) != 36 ||
        !json_equal(json_object_get(value, "executionManifest"), target->manifest))
        return -1;
    json_t *control = json_object_get(value, "helperControl"),
           *native = json_object_get(value, "native"),
           *receipt = json_object_get(value, "reconciliation");
    const char *stop = json_string_value(json_object_get(receipt, "stopOperationId"));
    json_int_t generation = json_integer_value(json_object_get(receipt, "controlGeneration")),
               pid = json_integer_value(json_object_get(native, "pid")),
               birth = json_integer_value(json_object_get(native, "startTime"));
    if (!helper_role(target, id, control) || !text(control, "stage", "created") ||
        !json_is_null(json_object_get(control, "result")) ||
        !json_is_null(json_object_get(control, "termination")) || !stop ||
        !gstd_identifier(stop, json_string_length(json_object_get(receipt, "stopOperationId"))) ||
        generation <= (json_int_t)target->generation || generation > GSTD_SAFE_INTEGER ||
        !helper_reconciled(value, stop, (uint64_t)generation) || !text(native, "kind", "libcrun") ||
        !json_is_object(native) || json_object_size(native) != 7 || !text(native, "id", id) ||
        !text(native, "bootId", boot) ||
        json_integer_value(json_object_get(native, "generation")) !=
            (json_int_t)target->generation ||
        !json_is_integer(json_object_get(native, "pid")) || pid <= 0 || pid > INT_MAX ||
        !json_is_integer(json_object_get(native, "startTime")) || birth <= 0 ||
        birth > GSTD_SAFE_INTEGER ||
        !text(native, "component",
              json_integer_value(json_object_get(control, "purpose")) == GSTD_EXEC_SCHEMA_MIGRATE
                  ? "api"
                  : "scylladb"))
        return -1;
    sqlite3_stmt *s = prepare(
        db, "SELECT 1 FROM operations o JOIN deployments d ON d.deployment_id=o.deployment_id "
            "WHERE o.operation_id=? AND o.method='deployment.stop' AND o.caller_uid=? AND "
            "o.deployment_id=? "
            "AND o.state IN ('running','succeeded','failed') AND d.generation>=?");
    int valid = s != NULL;
    if (s) {
        bind(s, 1, stop);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
        bind(s, 3, target->deployment_id);
        sqlite3_bind_int64(s, 4, generation);
        valid = sqlite3_step(s) == SQLITE_ROW;
    }
    sqlite3_finalize(s);
    if (!valid)
        return -1;
    return deleted_absent(id, native, boot, native_fd);
}
int gstd_migration_recovery_service_stopping(sqlite3 *db, const gstd_deployment_target *target,
                                             const char *stop, const gstd_native_context *context) {
    uint64_t generation;
    if (!context || context->control_owner != getpid() || context->expected_pid <= 0 ||
        !context->expected_start_time || !context->boot_id || strlen(context->boot_id) != 36 ||
        current(db, target, stop, &generation))
        return -1;
    char source[80];
    snprintf(source, sizeof(source), "run-scylladb-%s", target->operation_id);
    if (!context->backend_id || strcmp(source, context->backend_id) || !context->component ||
        strcmp(context->component, "scylladb"))
        return -1;
    json_t *value = owner(db, target, source);
    int valid = value && native_matches(json_object_get(value, "native"), target, context);
    for (unsigned i = 0; valid && i < 2; i++) {
        char id[80];
        snprintf(id, sizeof(id), "%s-%s", i ? "run-schema" : "run-cql", target->operation_id);
        json_t *helper = owner(db, target, id), *control = json_object_get(helper, "helperControl");
        if (!helper)
            valid = 0;
        else if (!control)
            valid = !json_object_get(helper, "native") && !json_object_get(helper, "population");
        else
            valid = helper_role(target, id, control) &&
                    (!gstd_deployment_helper_receipt_read(target, id, helper, context->boot_id,
                                                          context->state_fd) ||
                     !gstd_migration_recovery_helper_receipt_read(
                         db, target, id, helper, context->boot_id, context->state_fd));
        json_decref(helper);
    }
    int result = valid ? write_owner(db, target, source, value, "stopping") : -1;
    json_decref(value);
    return result;
}
int gstd_migration_recovery_finish(sqlite3 *db, const gstd_deployment_target *target,
                                   const char *stop) {
    uint64_t generation;
    if (current(db, target, stop, &generation))
        return -1;
    char source[80];
    snprintf(source, sizeof(source), "run-scylladb-%s", target->operation_id);
    json_t *value = owner(db, target, source);
    int untouched = value && !json_object_get(value, "native") &&
                    !json_object_get(value, "population") &&
                    !json_object_get(value, "helperControl");
    int valid = value && (untouched || !gstd_termination_read(db, target, source, value) ||
                          original_source_deleted(target, source, value));
    if (valid && untouched) {
        for (unsigned i = 0; valid && i < 2; i++) {
            char id[80];
            snprintf(id, sizeof(id), "%s-%s", i ? "run-schema" : "run-cql", target->operation_id);
            json_t *helper = owner(db, target, id);
            valid = helper && !json_object_get(helper, "native") &&
                    !json_object_get(helper, "population") &&
                    !json_object_get(helper, "helperControl");
            json_decref(helper);
        }
    }
    json_decref(value);
    if (!valid)
        return -1;
    sqlite3_stmt *s = prepare(
        db,
        "UPDATE resource_ownership SET ownership_state='released',"
        "lifecycle_stage=CASE WHEN resource_type='asset-root' THEN 'released' ELSE 'retained' END,"
        "observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE operation_id=? AND "
        "resource_id!='migration-'||operation_id AND ownership_state='held'");
    if (!s)
        return -1;
    bind(s, 1, target->operation_id);
    int changed = sqlite3_step(s) == SQLITE_DONE ? sqlite3_changes(db) : 0;
    valid = changed == 9 || (untouched && changed == 8);
    sqlite3_finalize(s);
    s = valid
            ? prepare(
                  db,
                  "SELECT 1 FROM resource_ownership WHERE operation_id=? AND "
                  "resource_id!='migration-'||operation_id AND ownership_state!='released' LIMIT 1")
            : NULL;
    valid = s != NULL;
    if (s) {
        bind(s, 1, target->operation_id);
        valid = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    for (unsigned i = 0; valid && i < 3; i++) {
        char id[80];
        snprintf(id, sizeof(id), "%s-%s",
                 i == 0   ? "asset-scylladb"
                 : i == 1 ? "asset-cql"
                          : "asset-schema",
                 target->operation_id);
        valid = !gstd_asset_record_unpin(db, id);
    }
    json_t *control = valid ? gstd_migration_inspect(db, target->operation_id) : NULL;
    valid = control && !json_object_set_new(control, "stage", json_string("failed"));
    char *bytes = valid ? gstd_json_dump(control) : NULL;
    json_decref(control);
    s = bytes
            ? prepare(db,
                      "UPDATE resource_ownership SET backend_identity=?,ownership_state='released',"
                      "lifecycle_stage='retained',observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') "
                      "WHERE "
                      "resource_id='migration-'||? AND operation_id=? AND ownership_state='held'")
            : NULL;
    valid = s != NULL;
    if (s) {
        bind(s, 1, bytes);
        bind(s, 2, target->operation_id);
        bind(s, 3, target->operation_id);
        valid = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    }
    sqlite3_finalize(s);
    free(bytes);
    s = valid ? prepare(db, "UPDATE operations SET recovery='normal' WHERE operation_id=? AND "
                            "state='failed' AND recovery='uncertain'")
              : NULL;
    valid = s != NULL;
    if (s) {
        bind(s, 1, target->operation_id);
        valid = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    }
    sqlite3_finalize(s);
    s = valid ? prepare(db, "UPDATE deployments SET "
                            "readiness='pending',observed_generation=generation,recovery='normal',"
                            "writer_ownership='released',observed_at=strftime('%Y-%m-%dT%H:%M:%SZ',"
                            "'now') WHERE deployment_id=? "
                            "AND generation=? AND last_operation_id=? AND desired_state='stopped' "
                            "AND recovery='uncertain'")
              : NULL;
    valid = s != NULL;
    if (s) {
        bind(s, 1, target->deployment_id);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)generation);
        bind(s, 3, stop);
        valid = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    }
    sqlite3_finalize(s);
    s = valid ? prepare(db, "UPDATE operations SET "
                            "state='succeeded',result='{\"stopped\":true,\"volumes\":\"retained\","
                            "\"migrationOutcomePreserved\":true}',error=NULL,updated_at=strftime('%"
                            "Y-%m-%dT%H:%M:%SZ','now') "
                            "WHERE operation_id=? AND caller_uid=? AND method='deployment.stop' "
                            "AND state='running' AND recovery='normal'")
              : NULL;
    valid = s != NULL;
    if (s) {
        bind(s, 1, stop);
        sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
        valid = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1;
    }
    sqlite3_finalize(s);
    return valid ? 0 : -1;
}
