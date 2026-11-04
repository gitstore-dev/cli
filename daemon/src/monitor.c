#define _POSIX_C_SOURCE 200809L
#include "monitor.h"
#include "gitstored/scheduler.h"
#include "native_exit.h"
#include <time.h>
#include <unistd.h>
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int kernel(gstd_deployment *d, int *component) {
    *component = -1;
    int status = 0;
    for (unsigned i = 0; i < d->count; i++) {
        int dead = gstd_native_watch_poll(d->workloads[i].death_fd);
        if (dead) {
            *component = i == 0 ? 1 : i == 1 ? 0 : 2;
            if (dead < 0)
                return -1;
            status = 1;
        }
    }
    int network = gstd_native_watch_poll(d->network.pid_fd),
        keeper = gstd_native_watch_poll(d->keeper_death_fd);
    return network < 0 || keeper < 0 ? -1 : status || network || keeper;
}
static int observation(gstd_deployment *d, int ready, int uncertain, int component) {
    sqlite3 *db = d->environment->database;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    sqlite3_stmt *s = NULL;
    int valid = sqlite3_prepare_v2(db,
                                   "UPDATE deployments SET readiness=?,observed_generation=CASE "
                                   "WHEN ? THEN generation ELSE observed_generation END,"
                                   "recovery=CASE WHEN ? THEN 'uncertain' ELSE recovery "
                                   "END,observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') "
                                   "WHERE deployment_id=? AND generation=? AND last_operation_id=? "
                                   "AND desired_state='running' AND recovery='normal'",
                                   -1, &s, NULL) == SQLITE_OK;
    if (valid) {
        sqlite3_bind_text(s, 1, ready ? "ready" : "failed", -1, SQLITE_STATIC);
        sqlite3_bind_int(s, 2, ready);
        sqlite3_bind_int(s, 3, uncertain);
        sqlite3_bind_text(s, 4, d->target.deployment_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 5, (sqlite3_int64)d->target.generation);
        sqlite3_bind_text(s, 6, d->target.operation_id, -1, SQLITE_TRANSIENT);
        valid = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    s = NULL;
    if (valid && component >= 0) {
        static const char *names[] = {"api", "git", "controller"};
        valid = component < 3 &&
                sqlite3_prepare_v2(
                    db,
                    "UPDATE components SET "
                    "observed_state=?,observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE "
                    "deployment_id=? AND generation=? AND component=? AND EXISTS(SELECT 1 FROM "
                    "deployments d WHERE "
                    "d.deployment_id=components.deployment_id AND d.last_operation_id=? AND "
                    "d.desired_state='running')",
                    -1, &s, NULL) == SQLITE_OK;
        if (valid) {
            sqlite3_bind_text(s, 1,
                              uncertain                              ? "unknown"
                              : d->monitor.last_ready[component] > 0 ? "running"
                                                                     : "failed",
                              -1, SQLITE_STATIC);
            sqlite3_bind_text(s, 2, d->target.deployment_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 3, (sqlite3_int64)d->target.generation);
            sqlite3_bind_text(s, 4, names[component], -1, SQLITE_STATIC);
            sqlite3_bind_text(s, 5, d->target.operation_id, -1, SQLITE_TRANSIENT);
            valid = sqlite3_step(s) == SQLITE_DONE;
        }
        sqlite3_finalize(s);
    }
    if (!valid || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    d->monitor.reported_ready = ready;
    return 0;
}
static int fresh(gstd_deployment *d, int64_t now) {
    for (unsigned i = 0; i < d->count; i++)
        if (d->monitor.last_ready[i] <= 0 || now - d->monitor.last_ready[i] > 30000)
            return 0;
    return 1;
}
int gstd_deployment_monitor_poll(gstd_deployment *d) {
    if (!d || d->recovered || d->outcome || !d->stage || !d->environment || !d->credential_source ||
        d->ready_at_ms <= 0)
        return -1;
    int64_t now = now_ms();
    if (now < 0)
        return -1;
    if (!d->monitor.initialized) {
        d->monitor.initialized = 1;
        d->monitor.reported_ready = 1;
        for (unsigned i = 0; i < d->count; i++)
            d->monitor.last_ready[i] = d->ready_at_ms;
        d->monitor.next_due = d->ready_at_ms + 5000;
    }
    int failed_component = -1;
    int alive = kernel(d, &failed_component);
    if (alive && failed_component >= 0)
        d->monitor.last_ready[failed_component] = 0;
    if (alive && d->monitor.reported_ready && observation(d, 0, alive < 0, failed_component))
        return -1;
    if (!fresh(d, now) && d->monitor.reported_ready && observation(d, 0, 0, -1))
        return -1;
    if (d->monitor.running) {
        if (!d->monitor.has_reply) {
            gstd_bootstrap_job_reply reply = {0};
            int done = gstd_bootstrap_job_poll(&d->bootstrap, -1, &reply);
            if (done < 0)
                return -1;
            if (!done)
                return 0;
            d->monitor.status = reply.uid[0] ? GSTD_BOOT_HTTP_REJECTED : reply.status;
            d->monitor.has_reply = 1;
        }
        /* A storage failure retains this lease/result for retry, never an
         * untracked worker or a manufactured operation completion. */
        if (gstd_schedule_finish(d->environment->database, d->target.operation_id))
            return -1;
        int index = d->monitor.index;
        d->monitor.last_ready[index] = d->monitor.status == GSTD_BOOT_HTTP_READY ? now : 0;
        d->monitor.running = 0;
        d->monitor.has_reply = 0;
        d->monitor.index = (index + 1) % (int)d->count;
        d->monitor.next_due = now + 5000;
        return observation(d, alive == 0 && fresh(d, now), alive < 0, index);
    }
    if (alive || now < d->monitor.next_due)
        return 0;
    int reserved = gstd_schedule_observer_reserve(d->environment->database, d->target.operation_id);
    if (reserved != 1) {
        d->monitor.next_due = now + 1000;
        return reserved < 0 ? -1 : 0;
    }
    static const gstd_bootstrap_http_action actions[] = {GSTD_BOOT_API_READY, GSTD_BOOT_GIT_READY,
                                                         GSTD_BOOT_CONTROLLER_READY};
    gstd_bootstrap_job_input input = {.kind = GSTD_BOOT_JOB_HTTP,
                                      .preparation = {.manifest = d->target.manifest,
                                                      .store_id = d->target.store_id,
                                                      .deployment_id = d->target.deployment_id,
                                                      .operation_id = d->target.operation_id,
                                                      .datastore = "memdb",
                                                      .generation = d->target.generation,
                                                      .control_owner = getpid(),
                                                      .scope_fd = d->configuration.descriptors[1]},
                                      .broker_fd = d->environment->broker_fd,
                                      .source = d->credential_source,
                                      .action = actions[d->monitor.index],
                                      .network = &d->network_observed.network};
    d->monitor.running = 1;
    if (gstd_bootstrap_job_launch(&input, 5000, &d->bootstrap)) {
        d->monitor.has_reply = 1;
        d->monitor.status = GSTD_BOOT_HTTP_UNCERTAIN;
    }
    return 0;
}
