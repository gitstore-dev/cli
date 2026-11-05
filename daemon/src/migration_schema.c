#define _POSIX_C_SOURCE 200809L
#include "migration_schema.h"
#include "migration.h"
#include "migration_history.h"
#include <poll.h>
#include <time.h>
#include <unistd.h>
enum { READY = 1, INSPECT, HISTORY, PREPARE, MUTATE, VERIFY, FINISHED };
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int finish(gstd_migration_schema *schema, int outcome) {
    schema->stage = FINISHED;
    schema->outcome = outcome;
    return 1;
}
static int transaction(gstd_migration_schema *schema, int prepare) {
    gstd_datastore *d = schema->datastore;
    sqlite3 *db = d->environment->database;
    if (gstd_datastore_service_live(d) ||
        sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    /* These observations derive from held writer/service capabilities and
     * completed, journaled image-native inspection; never from wire input. */
    gstd_migration_observation observed = {1, 1, 1, 1};
    int failed = prepare ? gstd_migration_prepare(db, d->target->operation_id, &observed)
                         : gstd_migration_transition(db, d->target->operation_id,
                                                     GSTD_MIG_DISPATCHED, GSTD_MIG_VERIFIED) ||
                               gstd_migration_transition(db, d->target->operation_id,
                                                         GSTD_MIG_VERIFIED, GSTD_MIG_CLEANUP);
    if (failed || sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return 0;
}
int gstd_migration_schema_begin(gstd_datastore *d, const gstd_network_namespace *network,
                                int resolver, gstd_migration_schema *schema) {
    int64_t now = now_ms();
    if (!d || !schema || schema->stage || !d->target || !d->target->migration || !d->environment ||
        !d->environment->database || d->index != 3 || d->outcome || d->helper.stage || !network ||
        resolver < 3 || now < 0 || !sqlite3_get_autocommit(d->environment->database) ||
        gstd_datastore_service_live(d) || gstd_network_namespace_check(network))
        return -1;
    *schema = (gstd_migration_schema){.datastore = d,
                                      .network = network,
                                      .resolver_fd = resolver,
                                      .output_fd = -1,
                                      .stage = READY,
                                      .readiness_deadline_ms = now + 60000};
    return 0;
}
int gstd_migration_schema_poll(gstd_migration_schema *schema, int cancellation) {
    if (!schema || !schema->stage || !schema->datastore ||
        (cancellation != -1 && cancellation < 3) ||
        !sqlite3_get_autocommit(schema->datastore->environment->database))
        return -1;
    if (schema->stage == FINISHED)
        return 1;
    gstd_datastore *d = schema->datastore;
    if (d->helper.stage) {
        int ready = gstd_datastore_helper_poll(d, cancellation);
        if (ready != 1)
            return ready;
        int outcome = d->helper.outcome;
        if (outcome == -2)
            return finish(schema, -2);
        if (gstd_datastore_helper_close(d))
            return finish(schema, -2);
        if (outcome) {
            if (schema->stage == READY) {
                int64_t now = now_ms();
                if (now < 0 || now >= schema->readiness_deadline_ms)
                    return finish(schema, -1);
                schema->next_readiness_ms = now + 1000;
                return 0;
            }
            return finish(schema, -1);
        }
        if (schema->stage == READY) {
            schema->stage = INSPECT;
            return 0;
        }
        if (schema->stage == MUTATE) {
            schema->stage = VERIFY;
            return 0;
        }
        json_t *sets =
            gstd_migration_output_rows(schema->output_fd, schema->stage == INSPECT ? 2 : 1);
        close(schema->output_fd);
        schema->output_fd = -1;
        int valid = sets != NULL;
        const char *version =
            json_string_value(json_object_get(d->target->manifest, "migrationTarget"));
        if (schema->stage == INSPECT)
            valid = valid && !gstd_migration_schema_inspection(sets, &schema->ledger_exists);
        else
            valid = valid && !gstd_migration_history_validate(version, json_array_get(sets, 0),
                                                              schema->stage == VERIFY);
        json_decref(sets);
        if (!valid)
            return finish(schema, -1);
        if (schema->stage == VERIFY)
            return finish(schema, transaction(schema, 0) ? -2 : 0);
        schema->stage = schema->stage == INSPECT && schema->ledger_exists ? HISTORY : PREPARE;
        return 0;
    }
    if (cancellation >= 3) {
        struct pollfd p = {.fd = cancellation, .events = POLLIN};
        int ready = poll(&p, 1, 0);
        if (ready < 0 || (ready && p.revents))
            return finish(schema, -1);
    }
    if (gstd_datastore_service_live(d))
        return finish(schema, -2);
    if (schema->stage == PREPARE) {
        if (transaction(schema, 1))
            return finish(schema, -2);
        schema->stage = MUTATE;
        return 0;
    }
    if (schema->stage == READY) {
        int64_t now = now_ms();
        if (now < 0 || now >= schema->readiness_deadline_ms)
            return finish(schema, -1);
        if (now < schema->next_readiness_ms)
            return 0;
    }
    gstd_execution_purpose purpose = schema->stage == READY     ? GSTD_EXEC_SCYLLA_READY
                                     : schema->stage == INSPECT ? GSTD_EXEC_SCHEMA_INSPECT
                                     : schema->stage == MUTATE  ? GSTD_EXEC_SCHEMA_MIGRATE
                                                                : GSTD_EXEC_SCHEMA_HISTORY;
    if (schema->stage == INSPECT || schema->stage == HISTORY || schema->stage == VERIFY) {
        schema->output_fd = gstd_migration_output_create();
        if (schema->output_fd < 3)
            return finish(schema, -1);
    }
    if (gstd_datastore_helper_begin(d, purpose, schema->network, d->service_boot,
                                    schema->resolver_fd, schema->output_fd, -1))
        return finish(schema, -2);
    return 0;
}
int gstd_migration_schema_close(gstd_migration_schema *schema) {
    if (!schema || schema->stage != FINISHED || schema->datastore->helper.helper.worker.worker)
        return -1;
    if (schema->output_fd >= 3)
        close(schema->output_fd);
    *schema = (gstd_migration_schema){0};
    return 0;
}
