#define _POSIX_C_SOURCE 200809L
#include "migration_recovery.h"
#include "gitstored/assets.h"
#include "migration.h"
#include "native_exit.h"
#include "native_recovery.h"
#include "network_restore.h"
#include "volume_records.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
enum {
    CONFIG = 1,
    VOLUME,
    WORKLOAD,
    NATIVE,
    NETWORK_SCOPE,
    NETWORK,
    ADOPT,
    HELPER_NEXT,
    HELPER_KILL_CREATED,
    HELPER_EXIT,
    HELPER_DELETE,
    SOURCE_STOP,
    WRITER,
    NETWORK_STOP,
    FINALIZE,
    FAILURE,
    FINISHED
};
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static sqlite3 *database(gstd_migration_recovery *m) { return m->environment->database; }
static sqlite3_stmt *prepare(gstd_migration_recovery *m, const char *sql) {
    sqlite3_stmt *s = NULL;
    return sqlite3_prepare_v2(database(m), sql, -1, &s, NULL) == SQLITE_OK ? s : NULL;
}
static void bind(sqlite3_stmt *s, int index, const char *value) {
    sqlite3_bind_text(s, index, value, -1, SQLITE_TRANSIENT);
}
static int commit(gstd_migration_recovery *m, int failed) {
    if (failed || sqlite3_exec(database(m), "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(database(m), "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}
static int failure(gstd_migration_recovery *m) {
    m->stage = FAILURE;
    m->outcome = -2;
    if (sqlite3_exec(database(m), "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return 0;
    sqlite3_stmt *s = prepare(
        m, "UPDATE resource_ownership SET ownership_state='uncertain',"
           "observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE operation_id=? AND "
           "ownership_state='held' AND "
           "EXISTS(SELECT 1 FROM deployments WHERE deployment_id=resource_ownership.deployment_id "
           "AND last_operation_id=? AND generation=?)");
    int failed = !s;
    if (s) {
        bind(s, 1, m->execution.target.operation_id);
        bind(s, 2, m->stop_operation);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)m->control_generation);
        failed = sqlite3_step(s) != SQLITE_DONE;
    }
    sqlite3_finalize(s);
    s = !failed ? prepare(m, "UPDATE operations SET state='failed',recovery='uncertain',"
                             "error='{\"code\":\"OWNERSHIP_UNCERTAIN\",\"stage\":\"recovery\","
                             "\"retryable\":false}',"
                             "updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE operation_id=? "
                             "AND method='deployment.stop' AND state='running'")
                : NULL;
    failed = !s;
    if (s) {
        bind(s, 1, m->stop_operation);
        failed = sqlite3_step(s) != SQLITE_DONE || sqlite3_changes(database(m)) != 1;
    }
    sqlite3_finalize(s);
    if (commit(m, failed))
        return 0;
    m->stage = FINISHED;
    return 1;
}
static int inventory(gstd_migration_recovery *m) {
    sqlite3_stmt *s =
        prepare(m, "SELECT resource_id,resource_type FROM resource_ownership WHERE operation_id=? "
                   "AND deployment_id=? AND ownership_state IN ('held','uncertain')");
    if (!s)
        return -1;
    bind(s, 1, m->execution.target.operation_id);
    bind(s, 2, m->execution.target.deployment_id);
    unsigned found = 0;
    int step, valid = 1;
    char control[80];
    snprintf(control, sizeof(control), "migration-%s", m->execution.target.operation_id);
    while ((step = sqlite3_step(s)) == SQLITE_ROW && valid) {
        const char *id = (const char *)sqlite3_column_text(s, 0),
                   *kind = (const char *)sqlite3_column_text(s, 1);
        unsigned bit = 0;
        if (id && kind) {
            if (!strcmp(id, control) && !strcmp(kind, "configuration"))
                bit = 1;
            if (!strcmp(id, m->configuration_id) && !strcmp(kind, "configuration"))
                bit = 2;
            if (!strcmp(id, m->datastore.volume_id) && !strcmp(kind, "datastore-volume"))
                bit = 4;
            if (!strcmp(id, m->network_id) && !strcmp(kind, "network"))
                bit = 512;
            for (unsigned i = 0; i < 3; i++) {
                if (!strcmp(id, m->datastore.workloads[i].resource) && !strcmp(kind, "workload"))
                    bit = 1u << (3 + 2 * i);
                if (!strcmp(id, m->datastore.workloads[i].asset_resource) &&
                    !strcmp(kind, "asset-root"))
                    bit = 1u << (4 + 2 * i);
            }
        }
        if (!bit || (found & bit))
            valid = 0;
        found |= bit;
    }
    sqlite3_finalize(s);
    m->network_present = (found & 512) != 0;
    valid = valid && step == SQLITE_DONE && (found == 511 || found == 1023);
    for (unsigned i = 0; valid && i < 3; i++) {
        json_t *manifest = gstd_asset_manifest_component(m->execution.target.manifest,
                                                         i == 2 ? "api" : "scylladb");
        char key[65];
        valid = manifest && !gstd_fingerprint(manifest, key);
        json_decref(manifest);
        s = valid
                ? prepare(m, "SELECT 1 FROM asset_refs r JOIN assets a ON a.asset_key=r.asset_key "
                             "WHERE r.resource_id=? AND r.asset_key=? AND a.state='active'")
                : NULL;
        valid = s != NULL;
        if (s) {
            bind(s, 1, m->datastore.workloads[i].asset_resource);
            bind(s, 2, key);
            valid = sqlite3_step(s) == SQLITE_ROW;
        }
        sqlite3_finalize(s);
    }
    return valid ? 0 : -1;
}
static int load(gstd_migration_recovery *m, const char *id, const char *kind) {
    json_decref(m->expected);
    m->expected = NULL;
    sqlite3_stmt *s = prepare(m, "SELECT backend_identity,lifecycle_stage FROM resource_ownership "
                                 "WHERE resource_id=? AND resource_type=? AND operation_id=? AND "
                                 "deployment_id=? AND ownership_state IN ('held','uncertain')");
    if (!s)
        return -1;
    bind(s, 1, id);
    bind(s, 2, kind);
    bind(s, 3, m->execution.target.operation_id);
    bind(s, 4, m->execution.target.deployment_id);
    int valid = sqlite3_step(s) == SQLITE_ROW;
    if (valid) {
        const char *stage = (const char *)sqlite3_column_text(s, 1);
        valid = stage && strlen(stage) < sizeof(m->lifecycle);
        if (valid)
            memcpy(m->lifecycle, stage, strlen(stage) + 1);
        if (sqlite3_column_type(s, 0) != SQLITE_NULL) {
            m->expected =
                gstd_json_parse(sqlite3_column_text(s, 0), (size_t)sqlite3_column_bytes(s, 0));
            valid = valid && m->expected;
        }
    }
    sqlite3_finalize(s);
    return valid ? 0 : -1;
}
static int scope(gstd_migration_recovery *m, const char *id, const char *kind, int stage) {
    if (load(m, id, kind) || !m->expected)
        return -1;
    const gstd_deployment_target *t = &m->execution.target;
    gstd_filesystem_input input = {.action = GSTD_FS_REOPEN_SCOPE,
                                   .manifest = t->manifest,
                                   .scope_expected = m->expected,
                                   .store_id = t->store_id,
                                   .operation_id = t->operation_id,
                                   .deployment_id = t->deployment_id,
                                   .generation = t->generation,
                                   .resource_id = id,
                                   .resource_kind = kind,
                                   .descriptors = {m->environment->resource_fd, -1, -1}};
    if (gstd_filesystem_launch(&input, 10000, &m->filesystem))
        return -1;
    m->stage = stage;
    return 0;
}
static int untouched(gstd_migration_recovery *m) {
    if (m->native_intent || m->mutation_dispatched)
        return 0;
    for (unsigned i = 0; i < 3; i++)
        if (m->datastore.workloads[i].launched || !m->datastore.workloads[i].exited ||
            m->datastore.workloads[i].native.expected_pid)
            return 0;
    return 1;
}
static int next_workload(gstd_migration_recovery *m) {
    if (++m->index < 3)
        return scope(m, m->datastore.workloads[m->index].resource, "workload", WORKLOAD)
                   ? failure(m)
                   : 0;
    if (!m->network_present) {
        if (!untouched(m))
            return failure(m);
        m->network_none = 1;
        m->stage = ADOPT;
        return 0;
    }
    if (load(m, m->network_id, "network"))
        return failure(m);
    if (!m->expected) {
        if (!untouched(m) || strcmp(m->lifecycle, "reserved"))
            return failure(m);
        m->network_none = 1;
        m->stage = ADOPT;
        return 0;
    }
    return scope(m, m->network_id, "network", NETWORK_SCOPE) ? failure(m) : 0;
}
int gstd_migration_recovery_begin(const gstd_deployment_environment *e, const char *migration,
                                  const char *stop, gstd_migration_recovery *m) {
    if (!m || m->stage || !e || !e->database || e->resource_fd < 3 || e->native_fd < 3 ||
        !migration || !stop || !gstd_identifier(stop, strnlen(stop, 65)) ||
        !gstd_identifier(migration, strnlen(migration, 65)) || !sqlite3_get_autocommit(e->database))
        return -1;
    int64_t now = now_ms();
    if (now < 0)
        return -1;
    *m = (gstd_migration_recovery){.environment = e,
                                   .keeper_death_fd = -1,
                                   .network = GSTD_NETWORK_WORKER_INITIAL,
                                   .observed = GSTD_NETWORK_OBSERVATION_INITIAL,
                                   .outcome = -2,
                                   .deadline_ms = now + 30000};
    memcpy(m->stop_operation, stop, strlen(stop) + 1);
    snprintf(m->configuration_id, sizeof(m->configuration_id), "cfg-%s", migration);
    snprintf(m->network_id, sizeof(m->network_id), "net-%s", migration);
    if (gstd_native_boot_id(m->boot) ||
        sqlite3_exec(e->database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    int failed = gstd_migration_recovery_claim(e->database, migration, stop, &m->execution,
                                               &m->control_generation);
    if (!failed) {
        json_t *control = gstd_migration_inspect(e->database, migration);
        failed = !control;
        m->mutation_dispatched = json_is_true(json_object_get(control, "unsafe"));
        json_decref(control);
    }
    if (!failed)
        failed = gstd_datastore_recovery_seed(e, &m->execution.target, m->configuration_id,
                                              &m->datastore);
    if (!failed)
        failed = gstd_scylla_volume_record_recovery_read(
            e->database, &e->volume_root, migration, m->datastore.volume_id,
            m->datastore.producer_id, &m->expected_volume);
    if (!failed)
        failed = inventory(m);
    if (commit(m, failed)) {
        gstd_migration_execution_clear(&m->execution);
        return -1;
    }
    if (scope(m, m->configuration_id, "configuration", CONFIG))
        (void)failure(m);
    return 0;
}
static int writer_launch(gstd_migration_recovery *m) {
    gstd_filesystem_input input = {.action = GSTD_FS_DATASTORE_VOLUME,
                                   .manifest = m->execution.target.manifest,
                                   .volume_root = m->environment->volume_root,
                                   .volume_id = m->datastore.volume_id,
                                   .producer_id = m->datastore.producer_id,
                                   .volume_existing = 1,
                                   .volume_expected = m->expected_volume,
                                   .descriptors = {m->environment->volume_root.root_fd,
                                                   m->environment->volume_root.lock_fd, -1}};
    m->stage = WRITER;
    return gstd_filesystem_launch(&input, 5000, &m->filesystem) ? failure(m) : 0;
}
int gstd_migration_recovery_poll(gstd_migration_recovery *m) {
    if (!m || !m->stage || !m->environment || !sqlite3_get_autocommit(database(m)))
        return -1;
    if (m->stage == FINISHED)
        return 1;
    if (m->stage == FAILURE)
        return failure(m);
    int64_t now = now_ms();
    if (m->stage != FINALIZE && (now < 0 || now >= m->deadline_ms) && !m->native.worker &&
        !m->filesystem.worker && !m->stop.native.worker)
        return failure(m);
    sqlite3 *db = database(m);
    const gstd_deployment_target *t = &m->execution.target;
    int ready;
    if (m->stage == NATIVE) {
        gstd_native_status status;
        gstd_native_observation actual = {0};
        ready = gstd_native_poll(&m->native, -1, &status, &actual);
        if (ready != 1)
            return ready;
        gstd_deployment_workload *w = &m->datastore.workloads[m->index];
        if (status != GSTD_NATIVE_OK || (m->discovering && (!actual.created || actual.running)))
            return failure(m);
        if (m->discovering) {
            w->native.expected_pid = actual.pid;
            w->native.expected_start_time = actual.start_time;
        }
        if (actual.pid != w->native.expected_pid ||
            actual.start_time != w->native.expected_start_time)
            return failure(m);
        if (actual.stopped) {
            if (m->discovering ||
                gstd_native_population_reopen(json_object_get(m->expected, "population"),
                                              w->resource, m->boot, &w->population) ||
                gstd_native_population_empty(&w->population) != 1)
                return failure(m);
            w->recovered_stopped = 1;
            w->exited = 1;
        } else {
            if ((!actual.created && !actual.running) ||
                gstd_native_watch_open(actual.pid, actual.start_time, m->boot, &w->death_fd) ||
                gstd_native_population_open(w->resource, actual.pid, actual.start_time, m->boot,
                                            &w->population) ||
                gstd_native_population_empty(&w->population) != 0)
                return failure(m);
            json_t *population = json_object_get(m->expected, "population");
            if (population) {
                json_t *current =
                    gstd_native_population_identity(&w->population, w->resource, m->boot);
                int matches = current && json_equal(current, population);
                json_decref(current);
                if (!matches)
                    return failure(m);
            } else
                m->discovered[m->index] = json_incref(m->expected);
        }
        w->observed = actual;
        w->launched = 1;
        return next_workload(m);
    }
    if (m->stage == ADOPT) {
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            return failure(m);
        int failed = inventory(m);
        sqlite3_stmt *s =
            !failed ? prepare(m, "UPDATE resource_ownership SET ownership_state='held',"
                                 "observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE "
                                 "operation_id=? AND ownership_state IN ('held','uncertain')")
                    : NULL;
        failed = !s;
        if (s) {
            bind(s, 1, t->operation_id);
            failed = sqlite3_step(s) != SQLITE_DONE ||
                     sqlite3_changes(db) != (m->network_present ? 10 : 9);
        }
        sqlite3_finalize(s);
        for (unsigned i = 0; !failed && i < 3; i++)
            if (m->discovered[i])
                failed = gstd_migration_recovery_native_discovered(
                    db, t, m->stop_operation, &m->datastore.workloads[i], m->discovered[i]);
        if (commit(m, failed) || gstd_datastore_service_recovered(&m->datastore, m->boot))
            return failure(m);
        m->index = 1;
        m->stage = HELPER_NEXT;
        return 0;
    }
    if (m->stage == HELPER_NEXT) {
        while (m->index < 3 && !m->datastore.workloads[m->index].launched)
            m->index++;
        if (m->index == 3) {
            if (!m->datastore.workloads[0].launched)
                return writer_launch(m);
            if (gstd_migration_stop_recovered_begin(&m->datastore, m->stop_operation,
                                                    m->control_generation, &m->stop))
                return failure(m);
            m->stage = SOURCE_STOP;
            return 0;
        }
        gstd_deployment_workload *w = &m->datastore.workloads[m->index];
        if (w->observed.created && !w->observed.running && !w->exited) {
            if (gstd_native_launch(&w->native, GSTD_NATIVE_KILL, 5000, &m->native))
                return failure(m);
            m->stage = HELPER_KILL_CREATED;
        } else
            m->stage = HELPER_EXIT;
        return 0;
    }
    if (m->stage == HELPER_KILL_CREATED || m->stage == HELPER_DELETE) {
        gstd_native_status status;
        gstd_native_observation observation;
        ready = gstd_native_poll(&m->native, -1, &status, &observation);
        if (ready != 1)
            return ready;
        if (status != GSTD_NATIVE_OK)
            return failure(m);
        if (m->stage == HELPER_KILL_CREATED) {
            m->stage = HELPER_EXIT;
            return 0;
        }
        gstd_deployment_workload *w = &m->datastore.workloads[m->index];
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            return failure(m);
        if (commit(m, gstd_migration_recovery_helper_deleted(db, t, m->stop_operation, w)))
            return failure(m);
        w->launched = 0;
        m->index++;
        m->stage = HELPER_NEXT;
        return 0;
    }
    if (m->stage == HELPER_EXIT) {
        gstd_deployment_workload *w = &m->datastore.workloads[m->index];
        int dead = w->recovered_stopped && w->exited && w->observed.stopped
                       ? 1
                       : gstd_native_watch_poll(w->death_fd);
        int empty = gstd_native_population_empty(&w->population);
        if (dead < 0 || empty < 0)
            return failure(m);
        if (!dead || !empty)
            return 0;
        gstd_native_exit exit;
        (void)gstd_native_exit_poll(w->native.expected_pid, w->native.expected_start_time, &exit);
        w->exited = 1;
        if (gstd_native_launch(&w->native, GSTD_NATIVE_DELETE, 10000, &m->native))
            return failure(m);
        m->stage = HELPER_DELETE;
        return 0;
    }
    if (m->stage == SOURCE_STOP) {
        ready = gstd_migration_stop_poll(&m->stop);
        if (ready != 1)
            return ready;
        return m->stop.outcome ? failure(m) : writer_launch(m);
    }
    if (m->stage == NETWORK_STOP) {
        int dead = gstd_native_watch_poll(m->network.pid_fd),
            keeper = gstd_native_watch_poll(m->keeper_death_fd);
        if (dead < 0 || keeper < 0)
            return failure(m);
        if (!dead || !keeper)
            return 0;
        gstd_native_exit exit;
        (void)gstd_native_exit_poll(m->network.pid, m->network.start, &exit);
        (void)gstd_native_exit_poll(m->observed.network.keeper_pid,
                                    m->observed.network.keeper_start, &exit);
        m->stage = FINALIZE;
        return 0;
    }
    if (m->stage == FINALIZE) {
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            return 0;
        if (commit(m, gstd_migration_recovery_finish(db, t, m->stop_operation)))
            return 0;
        m->stage = FINISHED;
        m->outcome = 0;
        return 1;
    }
    gstd_filesystem_reply reply = {0};
    ready = gstd_filesystem_poll(&m->filesystem, -1, &reply);
    if (ready != 1)
        return ready;
    if (reply.status) {
        gstd_filesystem_reply_close(&reply);
        return failure(m);
    }
    if (m->stage == CONFIG) {
        m->configuration = reply;
        gstd_filesystem_input input = {.action = GSTD_FS_OBSERVE_DATASTORE,
                                       .manifest = t->manifest,
                                       .volume_root = m->environment->volume_root,
                                       .volume_id = m->datastore.volume_id,
                                       .producer_id = m->datastore.producer_id,
                                       .volume_expected = m->expected_volume,
                                       .descriptors = {m->environment->volume_root.root_fd,
                                                       m->environment->volume_root.lock_fd, -1}};
        m->stage = VOLUME;
        return gstd_filesystem_launch(&input, 5000, &m->filesystem) ? failure(m) : 0;
    }
    if (m->stage == VOLUME) {
        m->datastore.volume = (gstd_volume_handle){
            .data_fd = reply.descriptors[0], .writer_fd = -1, .identity = reply.volume};
        m->index = 0;
        return scope(m, m->datastore.workloads[0].resource, "workload", WORKLOAD) ? failure(m) : 0;
    }
    if (m->stage == WORKLOAD) {
        gstd_deployment_workload *w = &m->datastore.workloads[m->index];
        if (reply.descriptor_count != 4) {
            gstd_filesystem_reply_close(&reply);
            return failure(m);
        }
        w->scope = reply;
        w->scope.descriptor_count = 3;
        w->rootfs_fd = reply.descriptors[3];
        if (json_object_get(m->expected, "native") ||
            json_object_get(m->expected, "helperControl") ||
            json_object_get(m->expected, "population"))
            m->native_intent = 1;
        int unused = !json_object_get(m->expected, "native") &&
                     !json_object_get(m->expected, "population") &&
                     !json_object_get(m->expected, "helperControl") &&
                     !strcmp(m->lifecycle, "reserved");
        if (unused) {
            struct stat st;
            errno = 0;
            if (!fstatat(m->environment->native_fd, w->resource, &st, AT_SYMLINK_NOFOLLOW) ||
                errno != ENOENT)
                return failure(m);
            w->exited = 1;
            return next_workload(m);
        }
        if (m->index && (json_object_get(m->expected, "reconciliation") ||
                         (json_is_string(json_object_get(
                              json_object_get(m->expected, "helperControl"), "stage")) &&
                          !strcmp(json_string_value(json_object_get(
                                      json_object_get(m->expected, "helperControl"), "stage")),
                                  "deleted")))) {
            if (gstd_deployment_helper_receipt_read(t, w->resource, m->expected, m->boot,
                                                    m->environment->native_fd) &&
                gstd_migration_recovery_helper_receipt_read(db, t, w->resource, m->expected,
                                                            m->boot, m->environment->native_fd))
                return failure(m);
            w->exited = 1;
            return next_workload(m);
        }
        if (!m->index && json_object_get(m->expected, "termination")) {
            if (gstd_migration_recovery_service_receipt_read(db, t, w->resource, m->expected,
                                                             m->boot, m->environment->native_fd))
                return failure(m);
            w->exited = 1;
            return next_workload(m);
        }
        gstd_native_observation recorded = {0};
        int kind = m->index ? gstd_migration_recovery_helper_active_read(
                                  t, w->resource, m->expected, m->boot, &recorded)
                            : gstd_native_recovery_read(m->expected, m->lifecycle, w->resource,
                                                        w->name, m->boot, t->generation, &recorded);
        if (kind != GSTD_RECOVERY_NATIVE_KNOWN && kind != GSTD_RECOVERY_NATIVE_DISCOVER)
            return failure(m);
        m->discovering = kind == GSTD_RECOVERY_NATIVE_DISCOVER;
        w->native = (gstd_native_context){.backend_id = w->resource,
                                          .component = w->name,
                                          .boot_id = m->boot,
                                          .state_fd = m->environment->native_fd,
                                          .bundle_fd = w->scope.descriptors[2],
                                          .rootfs_fd = w->rootfs_fd,
                                          .writer_lease_fd = -1,
                                          .stdout_fd = -1,
                                          .stderr_fd = -1,
                                          .control_owner = getpid(),
                                          .expected_pid = recorded.pid,
                                          .expected_start_time = recorded.start_time};
        if (gstd_native_launch(&w->native, GSTD_NATIVE_OBSERVE, 10000, &m->native))
            return failure(m);
        m->stage = NATIVE;
        return 0;
    }
    if (m->stage == NETWORK_SCOPE) {
        m->network_scope = reply;
        gstd_filesystem_input input = {.action = GSTD_FS_RECOVER_NETWORK,
                                       .manifest = t->manifest,
                                       .store_id = t->store_id,
                                       .operation_id = t->operation_id,
                                       .resource_id = m->network_id,
                                       .scope_expected = m->expected,
                                       .descriptors = {reply.descriptors[0], -1, -1}};
        m->stage = NETWORK;
        return gstd_filesystem_launch(&input, 5000, &m->filesystem) ? failure(m) : 0;
    }
    if (m->stage == NETWORK) {
        if (gstd_network_restore_adopt(&reply, m->expected, &m->network, &m->observed)) {
            gstd_filesystem_reply_close(&reply);
            return failure(m);
        }
        if (gstd_native_watch_open(m->observed.network.keeper_pid, m->observed.network.keeper_start,
                                   m->boot, &m->keeper_death_fd))
            return failure(m);
        m->stage = ADOPT;
        return 0;
    }
    if (m->stage == WRITER) {
        gstd_volume_handle_close(&m->datastore.volume);
        m->datastore.volume = (gstd_volume_handle){.data_fd = reply.descriptors[0],
                                                   .writer_fd = reply.descriptors[1],
                                                   .identity = reply.volume};
        if (m->network_none) {
            m->stage = FINALIZE;
            return 0;
        }
        if (gstd_network_worker_signal(&m->network, 0))
            return failure(m);
        m->stage = NETWORK_STOP;
        return 0;
    }
    gstd_filesystem_reply_close(&reply);
    return failure(m);
}
int gstd_migration_recovery_close(gstd_migration_recovery *m) {
    if (!m || m->stage != FINISHED || m->native.worker || m->filesystem.worker ||
        m->stop.native.worker)
        return -1;
    if (m->datastore.stage && gstd_datastore_observers_close(&m->datastore))
        return -1;
    json_decref(m->expected);
    for (unsigned i = 0; i < 3; i++)
        json_decref(m->discovered[i]);
    if (m->keeper_death_fd >= 3)
        close(m->keeper_death_fd);
    gstd_network_worker_observation_close(&m->observed);
    gstd_network_worker_close(&m->network);
    gstd_filesystem_reply_close(&m->network_scope);
    gstd_filesystem_reply_close(&m->configuration);
    gstd_migration_execution_clear(&m->execution);
    memset(m, 0, sizeof(*m));
    return 0;
}
