#define _POSIX_C_SOURCE 200809L
#include "migration_runtime.h"
#include "gitstored/asset_records.h"
#include "migration.h"
#include "native_exit.h"
#include "network_records.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
enum {
    CONFIG = 1,
    PREPARE,
    NETWORK_SCOPE,
    NETWORK_READY,
    RESOLVER,
    SERVICE,
    SCHEMA,
    STOP,
    WRITER,
    NETWORK_STOP,
    FINALIZE,
    FINISHED
};
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static sqlite3 *database(gstd_migration_runtime *m) { return m->environment->database; }
static int commit(gstd_migration_runtime *m, int failed) {
    sqlite3 *db = database(m);
    if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}
static int uncertain(gstd_migration_runtime *m) {
    m->outcome = -2;
    m->cleanup_observed = 0;
    m->stage = FINALIZE;
    return 0;
}
static gstd_filesystem_input scope_input(gstd_migration_runtime *m, const char *id,
                                         const char *kind) {
    const gstd_deployment_target *t = &m->execution.target;
    return (gstd_filesystem_input){.action = GSTD_FS_SCOPE,
                                   .manifest = t->manifest,
                                   .store_id = t->store_id,
                                   .operation_id = t->operation_id,
                                   .deployment_id = t->deployment_id,
                                   .generation = t->generation,
                                   .resource_id = id,
                                   .resource_kind = kind,
                                   .descriptors = {m->environment->resource_fd, -1, -1}};
}
int gstd_migration_runtime_begin(const gstd_deployment_environment *e, const char *operation,
                                 gstd_migration_runtime *m) {
    if (!m || m->stage || !operation || !gstd_identifier(operation, strnlen(operation, 65)) || !e ||
        !e->database || !e->settings || e->resource_fd < 3 || e->native_fd < 3 || e->cache_fd < 3 ||
        !sqlite3_get_autocommit(e->database))
        return -1;
    *m = (gstd_migration_runtime){.environment = e,
                                  .resolver_fd = -1,
                                  .keeper_death_fd = -1,
                                  .network = GSTD_NETWORK_WORKER_INITIAL,
                                  .observed = GSTD_NETWORK_OBSERVATION_INITIAL,
                                  .outcome = -2};
    if (gstd_native_boot_id(m->boot) ||
        sqlite3_exec(e->database, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    int failed = gstd_migration_execution_read(e->database, operation, &m->execution);
    int n = snprintf(m->configuration_id, sizeof(m->configuration_id), "cfg-%s", operation),
        k = snprintf(m->network_id, sizeof(m->network_id), "net-%s", operation);
    failed = failed || n < 1 || n >= (int)sizeof(m->configuration_id) || k < 1 ||
             k >= (int)sizeof(m->network_id) ||
             gstd_deployment_resource_reserve(e->database, &m->execution.target,
                                              m->configuration_id, "configuration");
    if (commit(m, failed)) {
        gstd_migration_execution_clear(&m->execution);
        return -1;
    }
    m->stage = CONFIG;
    gstd_filesystem_input input = scope_input(m, m->configuration_id, "configuration");
    if (gstd_filesystem_launch(&input, 10000, &m->filesystem))
        uncertain(m);
    return 0;
}
static int finalize(gstd_migration_runtime *m) {
    sqlite3 *db = database(m);
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return 0;
    int failed = 0;
    if (m->cleanup_observed) {
        sqlite3_stmt *s = NULL;
        failed =
            sqlite3_prepare_v2(db,
                               "UPDATE resource_ownership SET ownership_state='released',"
                               "lifecycle_stage=CASE WHEN resource_type='asset-root' THEN "
                               "'released' ELSE 'retained' END "
                               "WHERE operation_id=? AND resource_id!='migration-'||operation_id "
                               "AND ownership_state='held'",
                               -1, &s, NULL) != SQLITE_OK;
        if (!failed) {
            sqlite3_bind_text(s, 1, m->execution.target.operation_id, -1, SQLITE_TRANSIENT);
            failed = sqlite3_step(s) != SQLITE_DONE || sqlite3_changes(db) != 9;
        }
        sqlite3_finalize(s);
        for (unsigned i = 0; !failed && i < 3; i++)
            failed = gstd_asset_record_unpin(db, m->datastore.workloads[i].asset_resource);
    }
    failed = failed ||
             gstd_migration_finish(db, m->execution.target.operation_id,
                                   m->schema_succeeded && m->cleanup_observed, m->cleanup_observed);
    if (commit(m, failed))
        return 0;
    m->outcome = m->cleanup_observed ? (m->schema_succeeded ? 0 : -1) : -2;
    m->stage = FINISHED;
    return 1;
}
int gstd_migration_runtime_poll(gstd_migration_runtime *m, int cancellation) {
    if (!m || !m->stage || !m->environment || (cancellation != -1 && cancellation < 3) ||
        !sqlite3_get_autocommit(database(m)))
        return -1;
    if (m->stage == FINISHED)
        return 1;
    if (m->stage == FINALIZE)
        return finalize(m);
    sqlite3 *db = database(m);
    const gstd_deployment_target *t = &m->execution.target;
    int ready;
    if (m->stage == PREPARE) {
        ready = gstd_datastore_prepare_poll(&m->datastore, cancellation);
        if (ready != 1)
            return ready;
        if (m->datastore.outcome)
            return uncertain(m);
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            return uncertain(m);
        if (commit(m, gstd_deployment_resource_reserve(db, t, m->network_id, "network")))
            return uncertain(m);
        gstd_filesystem_input input = scope_input(m, m->network_id, "network");
        m->stage = NETWORK_SCOPE;
        if (gstd_filesystem_launch(&input, 10000, &m->filesystem))
            return uncertain(m);
        return 0;
    }
    if (m->stage == NETWORK_READY) {
        ready = gstd_network_worker_ready(&m->network, &m->observed);
        if (!ready) {
            int64_t now = now_ms();
            return now < 0 || now >= m->deadline_ms ? uncertain(m) : 0;
        }
        if (ready < 0 || sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            return uncertain(m);
        int failed = gstd_network_record_running(db, &m->network, &m->observed);
        for (unsigned i = 0; !failed && i < 3; i++)
            failed = gstd_network_attachment_reserve(db, &m->network, &m->observed,
                                                     m->datastore.workloads[i].resource);
        if (commit(m, failed) ||
            gstd_native_watch_open(m->observed.network.keeper_pid, m->observed.network.keeper_start,
                                   m->observed.network.boot_id, &m->keeper_death_fd))
            return uncertain(m);
        gstd_filesystem_input input = {.action = GSTD_FS_RESOLVER,
                                       .descriptors = {m->configuration.descriptors[0], -1, -1}};
        m->stage = RESOLVER;
        if (gstd_filesystem_launch(&input, 10000, &m->filesystem))
            return uncertain(m);
        return 0;
    }
    if (m->stage == SERVICE) {
        ready = gstd_datastore_service_poll(&m->datastore, cancellation);
        if (ready != 1)
            return ready;
        if (m->datastore.service_outcome ||
            gstd_migration_schema_begin(&m->datastore, &m->observed.network, m->resolver_fd,
                                        &m->schema))
            return uncertain(m);
        m->stage = SCHEMA;
        return 0;
    }
    if (m->stage == SCHEMA) {
        ready = gstd_migration_schema_poll(&m->schema, cancellation);
        if (ready != 1)
            return ready;
        if (m->schema.outcome == -2)
            return uncertain(m);
        m->schema_succeeded = m->schema.outcome == 0;
        if (gstd_migration_schema_close(&m->schema) ||
            gstd_migration_stop_begin(&m->datastore, &m->stop))
            return uncertain(m);
        m->stage = STOP;
        return 0;
    }
    if (m->stage == STOP) {
        ready = gstd_migration_stop_poll(&m->stop);
        if (ready != 1)
            return ready;
        if (m->stop.outcome)
            return uncertain(m);
        gstd_filesystem_input input = {.action = GSTD_FS_DATASTORE_VOLUME,
                                       .manifest = t->manifest,
                                       .volume_root = m->environment->volume_root,
                                       .volume_id = m->datastore.volume_id,
                                       .producer_id = m->datastore.producer_id,
                                       .volume_existing = 1,
                                       .volume_expected = m->datastore.volume.identity,
                                       .descriptors = {m->environment->volume_root.root_fd,
                                                       m->environment->volume_root.lock_fd, -1}};
        m->stage = WRITER;
        if (gstd_filesystem_launch(&input, 5000, &m->filesystem))
            return uncertain(m);
        return 0;
    }
    if (m->stage == NETWORK_STOP) {
        int dead = gstd_native_watch_poll(m->network.pid_fd),
            keeper = gstd_native_watch_poll(m->keeper_death_fd);
        int64_t now = now_ms();
        if (dead < 0 || keeper < 0 || now < 0 || now >= m->deadline_ms)
            return uncertain(m);
        if (!dead || !keeper)
            return 0;
        gstd_native_exit exit;
        (void)gstd_native_exit_poll(m->network.pid, m->network.start, &exit);
        (void)gstd_native_exit_poll(m->observed.network.keeper_pid,
                                    m->observed.network.keeper_start, &exit);
        m->cleanup_observed = 1;
        m->stage = FINALIZE;
        return 0;
    }
    gstd_filesystem_reply reply = {0};
    ready = gstd_filesystem_poll(&m->filesystem, m->stage == WRITER ? -1 : cancellation, &reply);
    if (ready != 1)
        return ready;
    if (reply.status) {
        gstd_filesystem_reply_close(&reply);
        return uncertain(m);
    }
    if (m->stage == CONFIG) {
        m->configuration = reply;
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            return uncertain(m);
        if (commit(m, gstd_deployment_scope_record(db, t, m->configuration_id, &reply)) ||
            gstd_datastore_prepare_begin(m->environment, t, m->configuration_id, &m->datastore))
            return uncertain(m);
        m->stage = PREPARE;
        return 0;
    }
    if (m->stage == NETWORK_SCOPE) {
        m->network_scope = reply;
        gstd_network_worker_request request = {.manifest = t->manifest,
                                               .store_id = t->store_id,
                                               .resource_id = m->network_id,
                                               .operation_id = t->operation_id,
                                               .scope_fd = reply.descriptors[1]};
        if (gstd_network_worker_prepare(&request, &m->network) != GSTD_NETWORK_OK ||
            sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            return uncertain(m);
        if (commit(m, gstd_network_record_bind(db, &request, &m->network)) ||
            gstd_network_worker_permit(&m->network))
            return uncertain(m);
        m->deadline_ms = now_ms() + 10000;
        m->stage = NETWORK_READY;
        return 0;
    }
    if (m->stage == RESOLVER) {
        m->resolver_fd = reply.descriptors[0];
        if (gstd_datastore_service_begin(&m->datastore, &m->observed.network, m->boot,
                                         m->resolver_fd, -1, -1))
            return uncertain(m);
        m->stage = SERVICE;
        return 0;
    }
    if (m->stage == WRITER) {
        gstd_volume_handle_close(&m->datastore.volume);
        m->datastore.volume = (gstd_volume_handle){.data_fd = reply.descriptors[0],
                                                   .writer_fd = reply.descriptors[1],
                                                   .identity = reply.volume};
        if (gstd_network_worker_signal(&m->network, 0))
            return uncertain(m);
        m->deadline_ms = now_ms() + 10000;
        m->stage = NETWORK_STOP;
        return 0;
    }
    gstd_filesystem_reply_close(&reply);
    return uncertain(m);
}
int gstd_migration_runtime_close(gstd_migration_runtime *m) {
    if (!m || m->stage != FINISHED || m->filesystem.worker || m->datastore.service_job.worker ||
        m->datastore.helper.helper.worker.worker || m->stop.native.worker)
        return -1;
    if (m->schema.stage && gstd_migration_schema_close(&m->schema))
        return -1;
    if (m->datastore.stage && gstd_datastore_observers_close(&m->datastore))
        return -1;
    if (m->resolver_fd >= 3)
        close(m->resolver_fd);
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
