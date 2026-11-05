#define _POSIX_C_SOURCE 200809L
#include "migration_stop.h"
#include "migration_recovery_records.h"
#include "native_exit.h"
#include "termination.h"
#include <string.h>
#include <time.h>
enum { TERM = 1, EXIT, DELETE, FINISHED };
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int finish(gstd_migration_stop *stop, int outcome) {
    stop->stage = FINISHED;
    stop->outcome = outcome;
    return 1;
}
static int begin(gstd_datastore *d, const char *operation, uint64_t generation,
                 gstd_migration_stop *stop) {
    int64_t now = now_ms();
    if (!d || !stop || stop->stage || !d->target || !d->target->migration || !d->environment ||
        !d->environment->database || d->helper.stage || d->service_job.worker || now < 0 ||
        !sqlite3_get_autocommit(d->environment->database) ||
        (operation && (!gstd_identifier(operation, strnlen(operation, 65)) ||
                       generation <= d->target->generation || generation > GSTD_SAFE_INTEGER)))
        return -1;
    gstd_deployment_workload *w = &d->workloads[0];
    int stopped = operation && w->launched && w->exited && w->recovered_stopped &&
                  w->observed.stopped && gstd_native_population_empty(&w->population) == 1;
    int created =
        operation && w->launched && !w->exited && w->observed.created && !w->observed.running &&
        gstd_native_watch_poll(w->death_fd) == 0 &&
        !gstd_native_population_matches(&w->population, w->resource, w->native.expected_pid,
                                        w->native.expected_start_time, d->service_boot);
    if (!stopped && !created && gstd_datastore_service_live(d))
        return -1;
    sqlite3 *db = d->environment->database;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    int failed =
        operation ? gstd_migration_recovery_service_stopping(db, d->target, operation, &w->native)
                  : gstd_migration_service_stopping(db, d->target, &w->native);
    if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    *stop = (gstd_migration_stop){.datastore = d, .stage = TERM, .deadline_ms = now + 30000};
    if (operation) {
        memcpy(stop->stop_operation, operation, strlen(operation) + 1);
        stop->control_generation = generation;
    }
    if (stopped)
        stop->stage = EXIT;
    else if (gstd_native_launch(&w->native, created ? GSTD_NATIVE_KILL : GSTD_NATIVE_TERM, 5000,
                                &stop->native))
        finish(stop, -2);
    return 0;
}
int gstd_migration_stop_begin(gstd_datastore *d, gstd_migration_stop *stop) {
    return begin(d, NULL, 0, stop);
}
int gstd_migration_stop_recovered_begin(gstd_datastore *d, const char *operation,
                                        uint64_t generation, gstd_migration_stop *stop) {
    return operation ? begin(d, operation, generation, stop) : -1;
}
int gstd_migration_stop_poll(gstd_migration_stop *stop) {
    if (!stop || !stop->stage || !stop->datastore ||
        !sqlite3_get_autocommit(stop->datastore->environment->database))
        return -1;
    if (stop->stage == FINISHED)
        return 1;
    gstd_deployment_workload *w = &stop->datastore->workloads[0];
    if (stop->stage == TERM || stop->stage == DELETE) {
        gstd_native_status status;
        gstd_native_observation observed;
        int ready = gstd_native_poll(&stop->native, -1, &status, &observed);
        if (ready != 1)
            return ready;
        if (status != GSTD_NATIVE_OK)
            return finish(stop, -2);
        if (stop->stage == DELETE) {
            sqlite3 *db = stop->datastore->environment->database;
            if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
                return finish(stop, -2);
            int failed;
            if (*stop->stop_operation) {
                gstd_deployment owner = {.environment = stop->datastore->environment,
                                         .target = *stop->datastore->target};
                memcpy(owner.boot, stop->datastore->service_boot, 37);
                failed = gstd_termination_record(&owner, w, stop->stop_operation,
                                                 stop->control_generation);
            } else
                failed = gstd_migration_service_deleted(db, stop->datastore->target, &w->native,
                                                        &w->population, w->death_fd);
            if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
                sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
                return finish(stop, -2);
            }
            w->launched = 0;
            if (gstd_datastore_service_retire(stop->datastore))
                return finish(stop, -2);
            return finish(stop, 0);
        }
        stop->stage = EXIT;
    }
    int64_t now = now_ms();
    if (now < 0 || now >= stop->deadline_ms)
        return finish(stop, -2);
    int dead = w->recovered_stopped && w->exited && w->observed.stopped
                   ? 1
                   : gstd_native_watch_poll(w->death_fd);
    int empty = gstd_native_population_empty(&w->population);
    if (dead < 0 || empty < 0)
        return finish(stop, -2);
    if (!dead || !empty)
        return 0;
    gstd_native_exit collected;
    (void)gstd_native_exit_poll(w->native.expected_pid, w->native.expected_start_time, &collected);
    w->exited = 1;
    if (gstd_native_launch(&w->native, GSTD_NATIVE_DELETE, 5000, &stop->native))
        return finish(stop, -2);
    stop->stage = DELETE;
    return 0;
}
