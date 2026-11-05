#define _POSIX_C_SOURCE 200809L
#include "migration.h"
#include "migration_targets.inc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *stages[] = {"preflight", "prepared",  "dispatched", "verified",
                               "cleanup",   "succeeded", "failed",     "uncertain"};
static int same(const json_t *v, const char *expected) {
    return expected && json_is_string(v) && json_string_length(v) == strlen(expected) &&
           !memcmp(json_string_value(v), expected, strlen(expected));
}
static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    return sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK ? s : NULL;
}
static void bind(sqlite3_stmt *s, int at, const char *text) {
    sqlite3_bind_text(s, at, text, -1, SQLITE_TRANSIENT);
}
static int finish(sqlite3_stmt *s) {
    if (!s)
        return -1;
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}
json_t *gstd_migration_target(const char *version) {
    if (!version || !gstd_identifier(version, strnlen(version, 65)))
        return NULL;
    json_t *catalogue = gstd_json_parse((const unsigned char *)migration_targets_json,
                                        sizeof(migration_targets_json) - 1),
           *found = NULL;
    if (catalogue && json_integer_value(json_object_get(catalogue, "schemaVersion")) == 1 &&
        same(json_object_get(catalogue, "serverRevision"),
             "06152d193e854bc643fa884daa8b9f30b0d75d0f")) {
        json_t *targets = json_object_get(catalogue, "targets");
        for (size_t i = 0; i < json_array_size(targets); i++) {
            json_t *target = json_array_get(targets, i),
                   *helper = json_object_get(target, "helper");
            if (same(json_object_get(target, "targetVersion"), version) &&
                same(json_object_get(target, "datastore"), "scylladb") &&
                same(json_object_get(target, "serverBackend"), "scylla") &&
                same(json_object_get(helper, "path"), "/app/gitctl") &&
                same(json_object_get(helper, "subcommand"), "migrate")) {
                if (found) {
                    json_decref(found);
                    found = NULL;
                    break;
                }
                found = json_deep_copy(target);
            }
        }
    }
    json_decref(catalogue);
    return found;
}
typedef struct {
    sqlite3_int64 generation;
    int stopped, normal, released;
    char operation[65];
    json_t *intent;
} snapshot;
static int snapshot_read(sqlite3 *db, const char *deployment, snapshot *out) {
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *s =
        prepare(db, "SELECT "
                    "d.generation,d.desired_state,d.recovery,d.writer_ownership,o.operation_id,o."
                    "intent,d.profile_id,d.profile_version FROM deployments d JOIN operations o ON "
                    "o.operation_id=(SELECT operation_id FROM operations WHERE "
                    "deployment_id=d.deployment_id AND method='deployment.start' ORDER BY rowid "
                    "DESC LIMIT 1) WHERE d.deployment_id=? AND o.caller_uid=?");
    if (!s)
        return -1;
    bind(s, 1, deployment);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
    int step = sqlite3_step(s), ok = 0;
    if (step == SQLITE_ROW) {
        const char *operation = (const char *)sqlite3_column_text(s, 4);
        out->generation = sqlite3_column_int64(s, 0);
        out->stopped = !strcmp((const char *)sqlite3_column_text(s, 1), "stopped");
        out->normal = !strcmp((const char *)sqlite3_column_text(s, 2), "normal");
        out->released = !strcmp((const char *)sqlite3_column_text(s, 3), "released");
        out->intent =
            gstd_json_parse(sqlite3_column_text(s, 5), (size_t)sqlite3_column_bytes(s, 5));
        ok = operation && gstd_identifier(operation, (size_t)sqlite3_column_bytes(s, 4)) &&
             out->generation > 0 && out->generation <= GSTD_SAFE_INTEGER && out->intent &&
             !strcmp((const char *)sqlite3_column_text(s, 6), "core") &&
             !strcmp((const char *)sqlite3_column_text(s, 7), "1");
        if (ok)
            snprintf(out->operation, sizeof(out->operation), "%s", operation);
    }
    sqlite3_finalize(s);
    if (step == SQLITE_DONE)
        return 0;
    if (!ok) {
        json_decref(out->intent);
        out->intent = NULL;
        return -1;
    }
    return 1;
}
gstd_accept_status gstd_migration_admit(sqlite3 *db, const gstd_request *r) {
    if (!db || !r || r->method != GSTD_MIGRATE || sqlite3_get_autocommit(db))
        return GSTD_ACCEPT_INVALID;
    const char *deployment = json_string_value(json_object_get(r->params, "deploymentId"));
    json_t *target =
        gstd_migration_target(json_string_value(json_object_get(r->params, "targetVersion")));
    if (!target || !json_is_true(json_object_get(r->params, "backupAcknowledged"))) {
        json_decref(target);
        return GSTD_ACCEPT_MIGRATION_PRECONDITION;
    }
    json_decref(target);
    snapshot current;
    int found = deployment ? snapshot_read(db, deployment, &current) : -1;
    if (found != 1)
        return found == 0 ? GSTD_ACCEPT_NOT_FOUND : GSTD_ACCEPT_DATABASE;
    int supported = same(json_object_get(current.intent, "datastore"), "scylladb");
    json_decref(current.intent);
    if (!supported)
        return GSTD_ACCEPT_UNSUPPORTED_DATASTORE;
    if (!current.normal)
        return GSTD_ACCEPT_OWNERSHIP_UNCERTAIN;
    if (!current.stopped || !current.released)
        return GSTD_ACCEPT_MIGRATION_CONFLICT;
    sqlite3_stmt *s = prepare(db, "SELECT 1 FROM resource_ownership WHERE deployment_id=? AND "
                                  "ownership_state='uncertain' LIMIT 1");
    if (!s)
        return GSTD_ACCEPT_DATABASE;
    bind(s, 1, deployment);
    int step = sqlite3_step(s);
    sqlite3_finalize(s);
    return step == SQLITE_DONE  ? GSTD_ACCEPT_NEW
           : step == SQLITE_ROW ? GSTD_ACCEPT_OWNERSHIP_UNCERTAIN
                                : GSTD_ACCEPT_DATABASE;
}
static int resource(const char *operation, char id[65]) {
    if (!operation || !gstd_identifier(operation, strnlen(operation, 65)))
        return -1;
    int n = snprintf(id, 65, "migration-%s", operation);
    return n > 0 && n < 65 ? 0 : -1;
}
int gstd_migration_reserve(sqlite3 *db, const gstd_request *r, const char *operation) {
    if (!db || !r || sqlite3_get_autocommit(db))
        return -1;
    char id[65], hash[65];
    if (resource(operation, id))
        return -1;
    const char *deployment = json_string_value(json_object_get(r->params, "deploymentId"));
    snapshot current;
    if (!deployment || snapshot_read(db, deployment, &current) != 1)
        return -1;
    json_t *target =
        gstd_migration_target(json_string_value(json_object_get(r->params, "targetVersion")));
    int valid = target && !gstd_fingerprint(target, hash);
    json_t *value =
        valid ? json_pack("{s:s,s:O,s:s,s:s,s:I,s:s,s:b}", "kind", "migration-control",
                          "targetVersion", json_object_get(target, "targetVersion"), "targetDigest",
                          hash, "sourceOperationId", current.operation, "generation",
                          (json_int_t)current.generation, "stage", "preflight", "unsafe", 0)
              : NULL;
    json_decref(target);
    json_decref(current.intent);
    char *bytes = value ? gstd_json_dump(value) : NULL;
    json_decref(value);
    sqlite3_stmt *s = bytes ? prepare(db, "INSERT INTO resource_ownership "
                                          "VALUES(?,'configuration',?,?,'reserved',?,'held',"
                                          "strftime('%Y-%m-%dT%H:%M:%SZ','now'))")
                            : NULL;
    if (s) {
        bind(s, 1, id);
        bind(s, 2, deployment);
        bind(s, 3, operation);
        bind(s, 4, bytes);
    }
    int rc = finish(s);
    free(bytes);
    return rc;
}
typedef struct {
    json_t *value;
    gstd_migration_stage stage;
    int running, terminal, held, released, succeeded;
    sqlite3_int64 current_generation;
    char deployment[65], id[65];
} control;
static void clear(control *c) {
    json_decref(c->value);
    c->value = NULL;
}
static int load(sqlite3 *db, const char *operation, control *c) {
    memset(c, 0, sizeof(*c));
    if (!db || resource(operation, c->id))
        return -1;
    sqlite3_stmt *s = prepare(
        db,
        "SELECT r.backend_identity,o.state,r.ownership_state,o.deployment_id,d.generation,o.intent "
        "FROM resource_ownership r JOIN operations o ON o.operation_id=r.operation_id JOIN "
        "deployments d ON d.deployment_id=o.deployment_id WHERE r.resource_id=? AND "
        "r.resource_type='configuration' AND r.deployment_id=o.deployment_id AND o.operation_id=? "
        "AND o.caller_uid=? AND o.method='migration.start'");
    if (!s)
        return -1;
    bind(s, 1, c->id);
    bind(s, 2, operation);
    sqlite3_bind_int64(s, 3, (sqlite3_int64)geteuid());
    json_t *intent = NULL;
    int valid = sqlite3_step(s) == SQLITE_ROW;
    if (valid) {
        c->value = gstd_json_parse(sqlite3_column_text(s, 0), (size_t)sqlite3_column_bytes(s, 0));
        const char *state = (const char *)sqlite3_column_text(s, 1),
                   *deployment = (const char *)sqlite3_column_text(s, 3);
        c->running = state && !strcmp(state, "running");
        c->succeeded = state && !strcmp(state, "succeeded");
        c->terminal = state && (!strcmp(state, "succeeded") || !strcmp(state, "failed") ||
                                !strcmp(state, "cancelled"));
        c->held = !strcmp((const char *)sqlite3_column_text(s, 2), "held");
        c->released = !strcmp((const char *)sqlite3_column_text(s, 2), "released");
        c->current_generation = sqlite3_column_int64(s, 4);
        intent = gstd_json_parse(sqlite3_column_text(s, 5), (size_t)sqlite3_column_bytes(s, 5));
        valid = c->value && deployment &&
                gstd_identifier(deployment, (size_t)sqlite3_column_bytes(s, 3));
        if (valid)
            snprintf(c->deployment, sizeof(c->deployment), "%s", deployment);
    }
    sqlite3_finalize(s);
    const char *version = json_string_value(json_object_get(c->value, "targetVersion"));
    const char *source = json_string_value(json_object_get(c->value, "sourceOperationId"));
    json_t *target = version ? gstd_migration_target(version) : NULL;
    char hash[65];
    valid = valid && target && !gstd_fingerprint(target, hash) && json_object_size(c->value) == 7 &&
            same(json_object_get(c->value, "kind"), "migration-control") &&
            same(json_object_get(c->value, "targetDigest"), hash) &&
            same(json_object_get(intent, "targetVersion"), version) &&
            json_is_true(json_object_get(intent, "backupAcknowledged")) && source &&
            gstd_identifier(source, strnlen(source, 65)) &&
            json_is_integer(json_object_get(c->value, "generation")) &&
            json_integer_value(json_object_get(c->value, "generation")) > 0 &&
            c->current_generation >= json_integer_value(json_object_get(c->value, "generation")) &&
            json_is_boolean(json_object_get(c->value, "unsafe"));
    int found = -1;
    for (unsigned i = 0; i < 8; i++)
        if (same(json_object_get(c->value, "stage"), stages[i]))
            found = (int)i;
    valid = valid && found >= 0;
    if (valid) {
        c->stage = (gstd_migration_stage)found;
        if (found < 5 &&
            json_is_true(json_object_get(c->value, "unsafe")) != (found >= GSTD_MIG_DISPATCHED))
            valid = 0;
        if (found == GSTD_MIG_SUCCEEDED &&
            (!c->succeeded || !c->released || !json_is_true(json_object_get(c->value, "unsafe"))))
            valid = 0;
    }
    json_decref(target);
    json_decref(intent);
    s = valid ? prepare(db, "SELECT 1 FROM operations WHERE operation_id=? AND deployment_id=? AND "
                            "caller_uid=? AND method='deployment.start' AND "
                            "json_extract(intent,'$.datastore')='scylladb'")
              : NULL;
    if (s) {
        bind(s, 1, source);
        bind(s, 2, c->deployment);
        sqlite3_bind_int64(s, 3, (sqlite3_int64)geteuid());
        valid = sqlite3_step(s) == SQLITE_ROW;
    } else
        valid = 0;
    sqlite3_finalize(s);
    if (!valid) {
        clear(c);
        return -1;
    }
    return 0;
}
json_t *gstd_migration_inspect(sqlite3 *db, const char *operation) {
    control c;
    return load(db, operation, &c) ? NULL : c.value;
}
gstd_accept_status gstd_migration_check(sqlite3 *db, const char *operation,
                                        const gstd_migration_observation *o) {
    control c;
    if (!o || o->keyspace_exists < 0 || o->keyspace_exists > 1 || o->schema_compatible < 0 ||
        o->schema_compatible > 1 || o->datastore_exclusive < 0 || o->datastore_exclusive > 1 ||
        o->previous_helper_terminated < 0 || o->previous_helper_terminated > 1 ||
        load(db, operation, &c))
        return GSTD_ACCEPT_INVALID;
    int current =
        c.running && c.held && c.stage == GSTD_MIG_PREFLIGHT &&
        c.current_generation == json_integer_value(json_object_get(c.value, "generation"));
    clear(&c);
    if (!current)
        return GSTD_ACCEPT_MIGRATION_CONFLICT;
    if (!o->previous_helper_terminated)
        return GSTD_ACCEPT_OWNERSHIP_UNCERTAIN;
    if (!o->keyspace_exists || !o->schema_compatible)
        return GSTD_ACCEPT_MIGRATION_PRECONDITION;
    return o->datastore_exclusive ? GSTD_ACCEPT_NEW : GSTD_ACCEPT_MIGRATION_CONFLICT;
}
static int update(sqlite3 *db, const char *operation, control *c, const char *ownership,
                  const char *lifecycle) {
    char *bytes = gstd_json_dump(c->value);
    sqlite3_stmt *s =
        bytes ? prepare(db, "UPDATE resource_ownership SET "
                            "backend_identity=?,ownership_state=?,lifecycle_stage=?,observed_at="
                            "strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE resource_id=? AND "
                            "operation_id=? AND deployment_id=? AND ownership_state='held'")
              : NULL;
    if (s) {
        bind(s, 1, bytes);
        bind(s, 2, ownership);
        bind(s, 3, lifecycle);
        bind(s, 4, c->id);
        bind(s, 5, operation);
        bind(s, 6, c->deployment);
    }
    int rc = finish(s), changed = sqlite3_changes(db);
    free(bytes);
    return !rc && changed == 1 ? 0 : -1;
}
int gstd_migration_cancel_queued(sqlite3 *db, const char *operation) {
    if (!db || sqlite3_get_autocommit(db))
        return -1;
    control c;
    if (load(db, operation, &c))
        return -1;
    int valid = !c.running && !c.terminal && c.held && c.stage == GSTD_MIG_PREFLIGHT &&
                !json_is_true(json_object_get(c.value, "unsafe")) &&
                c.current_generation == json_integer_value(json_object_get(c.value, "generation"));
    sqlite3_stmt *s =
        valid
            ? prepare(db, "SELECT 1 FROM operations o JOIN jobs j ON j.operation_id=o.operation_id "
                          "WHERE o.operation_id=? AND o.state='accepted' AND j.state IN "
                          "('queued','reserved') "
                          "AND (SELECT count(*) FROM resource_ownership r WHERE "
                          "r.operation_id=o.operation_id)=1")
            : NULL;
    if (s) {
        bind(s, 1, operation);
        valid = sqlite3_step(s) == SQLITE_ROW;
    } else
        valid = 0;
    sqlite3_finalize(s);
    valid = valid && !json_object_set_new(c.value, "stage", json_string("failed")) &&
            !update(db, operation, &c, "released", "retained");
    s = valid ? prepare(db, "UPDATE operations SET state='cancelled',result=NULL,"
                            "error='{\"code\":\"OPERATION_FAILED\",\"stage\":\"cancel\","
                            "\"retryable\":false}',"
                            "updated_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE operation_id=? "
                            "AND state='accepted'")
              : NULL;
    if (s)
        bind(s, 1, operation);
    int rc = finish(s), changed = sqlite3_changes(db);
    clear(&c);
    return valid && !rc && changed == 1 ? 0 : -1;
}
static int advance(sqlite3 *db, const char *operation, gstd_migration_stage from,
                   gstd_migration_stage to) {
    if (!db || sqlite3_get_autocommit(db) || from < GSTD_MIG_PREFLIGHT ||
        from >= GSTD_MIG_CLEANUP || to != from + 1)
        return -1;
    control c;
    if (load(db, operation, &c))
        return -1;
    int valid =
        c.running && c.held && c.stage == from &&
        c.current_generation == json_integer_value(json_object_get(c.value, "generation")) &&
        !json_object_set_new(c.value, "stage", json_string(stages[to])) &&
        !json_object_set_new(c.value, "unsafe", json_boolean(to >= GSTD_MIG_DISPATCHED));
    int rc =
        valid ? update(db, operation, &c, "held", to < GSTD_MIG_DISPATCHED ? "reserved" : "running")
              : -1;
    clear(&c);
    return rc;
}
int gstd_migration_prepare(sqlite3 *db, const char *operation,
                           const gstd_migration_observation *observed) {
    if (!db || sqlite3_get_autocommit(db) ||
        gstd_migration_check(db, operation, observed) != GSTD_ACCEPT_NEW)
        return -1;
    return advance(db, operation, GSTD_MIG_PREFLIGHT, GSTD_MIG_PREPARED);
}
int gstd_migration_transition(sqlite3 *db, const char *operation, gstd_migration_stage from,
                              gstd_migration_stage to) {
    return from == GSTD_MIG_PREFLIGHT ? -1 : advance(db, operation, from, to);
}
int gstd_migration_cancel_safe(sqlite3 *db, const char *operation) {
    control c;
    if (load(db, operation, &c))
        return -1;
    int safe = !c.terminal && c.held && c.stage < GSTD_MIG_DISPATCHED &&
               !json_is_true(json_object_get(c.value, "unsafe"));
    clear(&c);
    return safe;
}
int gstd_migration_finish(sqlite3 *db, const char *operation, int succeeded, int cleanup) {
    if (!db || sqlite3_get_autocommit(db) || (succeeded != 0 && succeeded != 1) ||
        (cleanup != 0 && cleanup != 1))
        return -1;
    control c;
    if (load(db, operation, &c))
        return -1;
    if (!c.running || !c.held ||
        c.current_generation != json_integer_value(json_object_get(c.value, "generation")) ||
        (succeeded && (!cleanup || c.stage != GSTD_MIG_CLEANUP))) {
        clear(&c);
        return -1;
    }
    gstd_migration_stage final = succeeded ? GSTD_MIG_SUCCEEDED
                                 : cleanup ? GSTD_MIG_FAILED
                                           : GSTD_MIG_UNCERTAIN;
    const char *code = !cleanup                        ? "OWNERSHIP_UNCERTAIN"
                       : c.stage < GSTD_MIG_DISPATCHED ? "MIGRATION_PRECONDITION"
                                                       : "OPERATION_FAILED";
    json_t *out = succeeded
                      ? json_pack("{s:O,s:O,s:b}", "targetVersion",
                                  json_object_get(c.value, "targetVersion"), "targetDigest",
                                  json_object_get(c.value, "targetDigest"), "migrationCompleted", 1)
                      : json_pack("{s:s,s:s,s:b,s:s}", "code", code, "stage", "migration",
                                  "retryable", 0, "message",
                                  "Migration did not reach verified schema and cleanup completion; "
                                  "inspect its recorded identity");
    char *bytes = out ? gstd_json_dump(out) : NULL;
    json_decref(out);
    int valid = bytes && !json_object_set_new(c.value, "stage", json_string(stages[final]));
    if (valid)
        valid = !update(db, operation, &c, cleanup ? "released" : "uncertain",
                        cleanup ? "retained" : "stopping");
    sqlite3_stmt *s =
        valid ? prepare(db,
                        succeeded
                            ? "UPDATE operations SET "
                              "state='succeeded',result=?,error=NULL,updated_at=strftime('%Y-%m-%"
                              "dT%H:%M:%SZ','now') WHERE operation_id=? AND state='running'"
                            : "UPDATE operations SET "
                              "state='failed',error=?,result=NULL,recovery=?,updated_at=strftime('%"
                              "Y-%m-%dT%H:%M:%SZ','now') WHERE operation_id=? AND state='running'")
              : NULL;
    if (s) {
        bind(s, 1, bytes);
        if (succeeded)
            bind(s, 2, operation);
        else {
            bind(s, 2, cleanup ? "normal" : "uncertain");
            bind(s, 3, operation);
        }
    }
    valid = s && !finish(s) && sqlite3_changes(db) == 1;
    free(bytes);
    s = valid ? prepare(db,
                        "UPDATE deployments SET readiness=?,recovery=CASE WHEN EXISTS(SELECT 1 "
                        "FROM resource_ownership r WHERE r.deployment_id=deployments.deployment_id "
                        "AND r.ownership_state='uncertain') THEN 'uncertain' ELSE 'normal' "
                        "END,observed_at=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE "
                        "deployment_id=? AND generation=?")
              : NULL;
    if (s) {
        bind(s, 1, succeeded ? "pending" : "failed");
        bind(s, 2, c.deployment);
        sqlite3_bind_int64(s, 3, c.current_generation);
    }
    int rc = s && !finish(s) && sqlite3_changes(db) == 1 ? 0 : -1;
    clear(&c);
    return valid ? rc : -1;
}
int gstd_migration_start_allowed(sqlite3 *db, const char *deployment) {
    if (!db || !deployment || !gstd_identifier(deployment, strnlen(deployment, 65)))
        return -1;
    sqlite3_stmt *s = prepare(db, "SELECT operation_id,state FROM operations WHERE deployment_id=? "
                                  "AND method='migration.start' ORDER BY rowid DESC LIMIT 1");
    if (!s)
        return -1;
    bind(s, 1, deployment);
    int step = sqlite3_step(s), succeeded = 0;
    char operation[65] = {0};
    if (step == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(s, 0),
                   *state = (const char *)sqlite3_column_text(s, 1);
        if (id && gstd_identifier(id, (size_t)sqlite3_column_bytes(s, 0)))
            snprintf(operation, sizeof(operation), "%s", id);
        succeeded = state && !strcmp(state, "succeeded");
    }
    sqlite3_finalize(s);
    if (step == SQLITE_DONE)
        return 1; /* fresh-volume proof is still the native caller's responsibility */
    if (step != SQLITE_ROW || !*operation)
        return -1;
    control c;
    if (load(db, operation, &c))
        return -1;
    int allowed = succeeded && c.stage == GSTD_MIG_SUCCEEDED && c.released;
    clear(&c);
    return allowed;
}
