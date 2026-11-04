#define _POSIX_C_SOURCE 200809L
#include "cleanup.h"
#include "gitstored/asset_records.h"
#include "gitstored/protocol.h"
#include "native_exit.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
enum { NEXT = 1, TERM, EXIT, DELETE_NATIVE, WRITER, NETWORK, DONE };
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static sqlite3 *database(gstd_cleanup *c) { return c->deployment->environment->database; }
static int sql(gstd_cleanup *c, const char *s) {
    return sqlite3_exec(database(c), s, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
static sqlite3_stmt *prepare(gstd_cleanup *c, const char *text) {
    sqlite3_stmt *s = NULL;
    return sqlite3_prepare_v2(database(c), text, -1, &s, NULL) == SQLITE_OK ? s : NULL;
}
static void bind(sqlite3_stmt *s, int i, const char *text) {
    sqlite3_bind_text(s, i, text, -1, SQLITE_TRANSIENT);
}
static int commit(gstd_cleanup *c, int failure) {
    if (failure || sql(c, "COMMIT")) {
        sql(c, "ROLLBACK");
        return -1;
    }
    return 0;
}
static int stage_record(gstd_cleanup *c, const char *id, const char *stage) {
    if (sql(c, "BEGIN IMMEDIATE"))
        return -1;
    sqlite3_stmt *s =
        prepare(c, "UPDATE resource_ownership SET "
                   "lifecycle_stage=?,observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE "
                   "resource_id=? AND operation_id=? AND ownership_state='held'");
    int ok = s != NULL;
    if (ok) {
        bind(s, 1, stage);
        bind(s, 2, id);
        bind(s, 3, c->deployment->target.operation_id);
        ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(database(c)) == 1;
    }
    sqlite3_finalize(s);
    return commit(c, !ok);
}
static int finish(gstd_cleanup *c, int uncertain) {
    c->stage = DONE;
    c->outcome = uncertain ? -2 : 0;
    if (!*c->operation_id)
        return 1;
    if (sql(c, "BEGIN IMMEDIATE")) {
        c->outcome = -2;
        return 1;
    }
    gstd_deployment *d = c->deployment;
    int ok = 1;
    sqlite3_stmt *s = NULL;
    if (!uncertain) {
        s = prepare(
            c,
            "UPDATE resource_ownership SET "
            "ownership_state='released',lifecycle_stage=CASE WHEN resource_type='asset-root' "
            "THEN 'released' ELSE 'retained' END,observed_at=strftime('%Y-%m-%dT%"
            "H:%M:%SZ','now') WHERE operation_id=? AND deployment_id=? AND ownership_state='held'");
        ok = s != NULL;
        if (ok) {
            bind(s, 1, d->target.operation_id);
            bind(s, 2, d->target.deployment_id);
            ok = sqlite3_step(s) == SQLITE_DONE;
        }
        sqlite3_finalize(s);
        for (unsigned i = 0; ok && i < d->count; i++) {
            /* There may be no reference when a known early start failed. */
            s = prepare(c, "SELECT 1 FROM asset_refs WHERE resource_id=?");
            int exists = 0;
            ok = s != NULL;
            if (ok) {
                bind(s, 1, d->workloads[i].asset_resource);
                int n = sqlite3_step(s);
                exists = n == SQLITE_ROW;
                ok = exists || n == SQLITE_DONE;
            }
            sqlite3_finalize(s);
            if (ok && exists)
                ok = !gstd_asset_record_unpin(database(c), d->workloads[i].asset_resource);
        }
        s = ok ? prepare(c,
                         "UPDATE components SET "
                         "desired_state='stopped',observed_state='stopped',generation=?,observed_"
                         "at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE deployment_id=?")
               : NULL;
        ok = ok && s;
        if (ok) {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)c->generation);
            bind(s, 2, d->target.deployment_id);
            ok = sqlite3_step(s) == SQLITE_DONE;
        }
        sqlite3_finalize(s);
    }
    s = ok ? prepare(c, "UPDATE deployments SET readiness=?,observed_generation=CASE WHEN ?=0 THEN "
                        "generation ELSE observed_generation "
                        "END,recovery=?,writer_ownership=?,observed_at=strftime('%Y-%m-%dT%H:%M:%"
                        "SZ','now') WHERE deployment_id=? AND generation=? AND last_operation_id=?")
           : NULL;
    ok = ok && s;
    if (ok) {
        bind(s, 1, uncertain ? "failed" : "pending");
        sqlite3_bind_int(s, 2, uncertain);
        bind(s, 3, uncertain ? "uncertain" : "normal");
        bind(s, 4, uncertain ? "uncertain" : "released");
        bind(s, 5, d->target.deployment_id);
        sqlite3_bind_int64(s, 6, (sqlite3_int64)c->generation);
        bind(s, 7, c->operation_id);
        ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(database(c)) == 1;
    }
    sqlite3_finalize(s);
    s = ok ? prepare(c, "UPDATE operations SET "
                        "state=?,result=?,error=?,updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') "
                        "WHERE operation_id=? AND state IN ('accepted','running')")
           : NULL;
    ok = ok && s;
    if (ok) {
        bind(s, 1, uncertain ? "failed" : "succeeded");
        if (uncertain) {
            sqlite3_bind_null(s, 2);
            bind(s, 3,
                 "{\"code\":\"OWNERSHIP_UNCERTAIN\",\"stage\":\"stop\",\"retryable\":false,"
                 "\"message\":\"Termination or writer ownership could not be proven; resources "
                 "retained\"}");
        } else {
            bind(s, 2, "{\"stopped\":true,\"volumes\":\"retained\"}");
            sqlite3_bind_null(s, 3);
        }
        bind(s, 4, c->operation_id);
        ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(database(c)) == 1;
    }
    sqlite3_finalize(s);
    if (commit(c, !ok))
        c->outcome = -2;
    /* Close only our now-proven free writer; durable data remains unchanged. */
    if (!c->outcome && d->volume.writer_fd >= 3) {
        close(d->volume.writer_fd);
        d->volume.writer_fd = -1;
    }
    return 1;
}
int gstd_cleanup_begin(gstd_deployment *d, const char *operation, gstd_cleanup *c) {
    if (!d || !c || c->stage || (d->recovered && !d->recovery_proven) || d->filesystem.worker ||
        d->bootstrap.worker || d->native_job.worker || d->asset.worker.worker ||
        gstd_deployment_poll(d, -1) != 1 || !sqlite3_get_autocommit(d->environment->database))
        return -1;
    uint64_t current_generation = d->recovered ? d->recovery_generation : d->target.generation;
    const char *current_operation = d->recovered ? d->recovery_operation : d->target.operation_id;
    if (!current_generation || current_generation >= GSTD_SAFE_INTEGER ||
        !gstd_identifier(current_operation, strnlen(current_operation, 65)))
        return -1;
    *c = (gstd_cleanup){.deployment = d,
                        .index = (int)d->count - 1,
                        .uncertain = d->outcome == -2,
                        .deadline_ms = now_ms() + 30000};
    if (operation) {
        if (!gstd_identifier(operation, strnlen(operation, 65)) || strlen(operation) > 64)
            return -1;
        if (sql(c, "BEGIN IMMEDIATE"))
            return -1;
        sqlite3_stmt *s = prepare(
            c, "SELECT 1 FROM operations o JOIN deployments d ON d.deployment_id=o.deployment_id "
               "WHERE o.operation_id=? AND (o.method='deployment.stop' OR "
               "(o.method='operation.cancel' AND o.target_operation_id=d.last_operation_id)) "
               "AND o.caller_uid=? AND "
               "o.state IN ('accepted','running') AND d.deployment_id=? AND d.generation=? AND "
               "d.last_operation_id=?");
        int ok = s != NULL;
        if (ok) {
            bind(s, 1, operation);
            sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
            bind(s, 3, d->target.deployment_id);
            sqlite3_bind_int64(s, 4, (sqlite3_int64)current_generation);
            bind(s, 5, current_operation);
            ok = sqlite3_step(s) == SQLITE_ROW;
        }
        sqlite3_finalize(s);
        c->generation = current_generation + 1;
        s = ok ? prepare(c, "UPDATE deployments SET "
                            "generation=?,desired_state='stopped',readiness='pending',last_"
                            "operation_id=?,observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE "
                            "deployment_id=? AND generation=?")
               : NULL;
        ok = ok && s;
        if (ok) {
            sqlite3_bind_int64(s, 1, (sqlite3_int64)c->generation);
            bind(s, 2, operation);
            bind(s, 3, d->target.deployment_id);
            sqlite3_bind_int64(s, 4, (sqlite3_int64)current_generation);
            ok = sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(database(c)) == 1;
        }
        sqlite3_finalize(s);
        if (commit(c, !ok))
            return -1;
        memcpy(c->operation_id, operation, strlen(operation) + 1);
        if (d->recovered) {
            d->recovery_generation = c->generation;
            memcpy(d->recovery_operation, operation, strlen(operation) + 1);
        }
    }
    c->stage = NEXT;
    return 0;
}
int gstd_cleanup_poll(gstd_cleanup *c) {
    if (!c || !c->stage)
        return -1;
    if (c->stage == DONE)
        return 1;
    gstd_deployment *d = c->deployment;
    if (now_ms() >= c->deadline_ms && !c->native.worker && !c->filesystem.worker)
        return finish(c, 1);
    if (c->stage == NEXT) {
        while (c->index >= 0 && !d->workloads[c->index].launched)
            c->index--;
        if (c->index < 0) {
            if (d->volume.data_fd >= 3) {
                gstd_filesystem_input input = {.action = GSTD_FS_VOLUME,
                                               .volume_root = d->environment->volume_root,
                                               .volume_id = d->volume_id,
                                               .producer_id = d->producer_id,
                                               .volume_existing = 1,
                                               .volume_expected = d->volume.identity,
                                               .descriptors = {d->environment->volume_root.root_fd,
                                                               d->environment->volume_root.lock_fd,
                                                               -1}};
                /* A pre-launch failure may still hold the parent's own writer. */
                if (d->volume.writer_fd >= 3) {
                    close(d->volume.writer_fd);
                    d->volume.writer_fd = -1;
                }
                if (gstd_filesystem_launch(&input, 5000, &c->filesystem))
                    return finish(c, 1);
                c->stage = WRITER;
                return 0;
            }
            c->stage = NETWORK;
        } else {
            gstd_deployment_workload *w = &d->workloads[c->index];
            if (stage_record(c, w->resource, "stopping"))
                return finish(c, 1);
            int dead = w->death_fd >= 3 ? gstd_native_watch_poll(w->death_fd) : 0;
            if (dead < 0)
                return finish(c, 1);
            if (dead == 1) {
                c->stage = EXIT;
                return 0;
            }
            if (gstd_native_launch(&w->native, GSTD_NATIVE_TERM, 5000, &c->native))
                return finish(c, 1);
            c->stage = TERM;
            return 0;
        }
    }
    if (c->stage == TERM || c->stage == DELETE_NATIVE) {
        gstd_native_status status;
        gstd_native_observation observation;
        int ready = gstd_native_poll(&c->native, -1, &status, &observation);
        if (ready < 0)
            return -1;
        if (!ready)
            return 0;
        if (status != GSTD_NATIVE_OK)
            return finish(c, 1);
        if (c->stage == TERM) {
            c->stage = EXIT;
            return 0;
        }
        gstd_deployment_workload *w = &d->workloads[c->index];
        if (stage_record(c, w->resource, "retained"))
            return finish(c, 1);
        gstd_native_population_close(&w->population);
        w->launched = 0;
        c->index--;
        c->stage = NEXT;
        return 0;
    }
    if (c->stage == EXIT) {
        gstd_deployment_workload *w = &d->workloads[c->index];
        if (!w->exited) {
            gstd_native_exit result;
            int exited = w->death_fd >= 3
                             ? gstd_native_watch_poll(w->death_fd)
                             : gstd_native_exit_poll(w->native.expected_pid,
                                                     w->native.expected_start_time, &result);
            if (w->death_fd >= 3 && exited == 1)
                (void)gstd_native_exit_poll(w->native.expected_pid, w->native.expected_start_time,
                                            &result);
            if (exited < 0)
                return finish(c, 1);
            if (!exited)
                return 0;
            w->exited = 1;
        }
        int empty = gstd_native_population_empty(&w->population);
        if (empty < 0)
            return finish(c, 1);
        if (!empty)
            return 0;
        if (gstd_native_launch(&w->native, GSTD_NATIVE_DELETE, 5000, &c->native))
            return finish(c, 1);
        c->stage = DELETE_NATIVE;
        return 0;
    }
    if (c->stage == WRITER) {
        gstd_filesystem_reply reply = {0};
        int ready = gstd_filesystem_poll(&c->filesystem, -1, &reply);
        if (ready < 0)
            return -1;
        if (!ready)
            return 0;
        if (reply.status) {
            gstd_filesystem_reply_close(&reply);
            return finish(c, 1);
        }
        gstd_volume_handle_close(&d->volume);
        d->volume = (gstd_volume_handle){.data_fd = reply.descriptors[0],
                                         .writer_fd = reply.descriptors[1],
                                         .identity = reply.volume};
        c->stage = NETWORK;
    }
    if (c->stage == NETWORK) {
        if (d->network.pid > 0) {
            if (!c->started) {
                int dead = d->network.pid_fd >= 3 ? gstd_native_watch_poll(d->network.pid_fd) : 0;
                if (dead < 0 || (!dead && gstd_network_worker_signal(&d->network, 0)))
                    return finish(c, 1);
                c->started = 1;
            }
            gstd_native_exit result;
            int exited = d->network.pid_fd >= 3
                             ? gstd_native_watch_poll(d->network.pid_fd)
                             : gstd_native_exit_poll(d->network.pid, d->network.start, &result);
            if (d->network.pid_fd >= 3 && exited == 1) {
                int keeper = d->network.permitted ? gstd_native_watch_poll(d->keeper_death_fd) : 1;
                if (keeper < 0)
                    return finish(c, 1);
                if (!keeper)
                    return 0;
                (void)gstd_native_exit_poll(d->network.pid, d->network.start, &result);
                (void)gstd_native_exit_poll(d->network_observed.network.keeper_pid,
                                            d->network_observed.network.keeper_start, &result);
            }
            if (exited < 0)
                return finish(c, 1);
            if (!exited)
                return 0;
            gstd_network_worker_observation_close(&d->network_observed);
            gstd_network_worker_close(&d->network);
        }
        return finish(c, c->uncertain);
    }
    return 0;
}
int gstd_cleanup_dispose(gstd_cleanup *c) {
    if (!c || c->stage != DONE || c->outcome || c->native.worker || c->filesystem.worker)
        return -1;
    gstd_deployment *d = c->deployment;
    if (d->filesystem.worker || d->bootstrap.worker || d->native_job.worker ||
        d->asset.worker.worker)
        return -1;
    for (unsigned i = 0; i < d->count; i++)
        if (d->workloads[i].launched)
            return -1;
    if (gstd_deployment_observers_close(d))
        return -1;
    memset(c, 0, sizeof(*c));
    return 0;
}
