#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/operations.h"
#include "gitstored/scheduler.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif
static char *canonical(const json_t *value) {
    size_t length = json_dumpb(value, NULL, 0, JSON_COMPACT | JSON_SORT_KEYS);
    if (!length || length > GSTD_FRAME_MAX)
        return NULL;
    char *bytes = malloc(length + 1);
    if (!bytes)
        return NULL;
    if (json_dumpb(value, bytes, length, JSON_COMPACT | JSON_SORT_KEYS) != length) {
        free(bytes);
        return NULL;
    }
    bytes[length] = 0;
    return bytes;
}
int gstd_fingerprint(const json_t *intent, char output[65]) {
    char *bytes = canonical(intent);
    if (!bytes)
        return -1;
    unsigned char digest[32];
#ifdef __APPLE__
    unsigned char *result = CC_SHA256(bytes, (CC_LONG)strlen(bytes), digest);
#else
    unsigned char *result = SHA256((const unsigned char *)bytes, strlen(bytes), digest);
#endif
    free(bytes);
    if (!result)
        return -1;
    for (size_t i = 0; i < 32; ++i)
        snprintf(output + 2 * i, 3, "%02x", digest[i]);
    return 0;
}
static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return NULL;
    return s;
}
static void text(sqlite3_stmt *s, int at, const char *value) {
    if (value)
        sqlite3_bind_text(s, at, value, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(s, at);
}
static int finish(sqlite3_stmt *s) {
    if (!s)
        return -1;
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}
static int execute(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
static const char *method_name(gstd_method method) {
    switch (method) {
    case GSTD_START:
        return "deployment.start";
    case GSTD_STOP:
        return "deployment.stop";
    case GSTD_CANCEL:
        return "operation.cancel";
    case GSTD_MIGRATE:
        return "migration.start";
    default:
        return NULL;
    }
}
static int operation_identity(char output[65]) {
    unsigned char random[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t have = 0;
    while (have < sizeof(random)) {
        ssize_t n = read(fd, random + have, sizeof(random) - have);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return -1;
        }
        have += (size_t)n;
    }
    close(fd);
    memcpy(output, "op-", 3);
    for (size_t i = 0; i < sizeof(random); ++i)
        snprintf(output + 3 + 2 * i, 3, "%02x", random[i]);
    return 0;
}
gstd_accept_status gstd_accept(sqlite3 *db, const gstd_request *r, const json_t *defaults,
                               const char *volume, char operation[65]) {
    operation[0] = 0;
    const char *method = method_name(r->method);
    if (!method || !r->idempotency_key || !r->request_id ||
        (volume && !gstd_identifier(volume, strlen(volume))))
        return GSTD_ACCEPT_INVALID;
    json_t *intent = json_deep_copy(r->params);
    if (!intent)
        return GSTD_ACCEPT_DATABASE;
    if (r->method == GSTD_START) {
        if (!json_object_get(intent, "components")) {
            if (!json_is_array(defaults) ||
                json_object_set(intent, "components", (json_t *)defaults)) {
                json_decref(intent);
                return GSTD_ACCEPT_INVALID;
            }
        }
        if (!json_object_get(intent, "datastore"))
            json_object_set_new(intent, "datastore", json_string("memdb"));
        if (!json_object_get(intent, "allowProvision"))
            json_object_set_new(intent, "allowProvision", json_false());
    }
    json_t *check = json_pack("{s:{s:i,s:i},s:s,s:s,s:o,s:s}", "version", "major", 1, "minor", 0,
                              "requestId", r->request_id, "method", method, "params",
                              json_incref(intent), "idempotencyKey", r->idempotency_key);
    gstd_request validated;
    if (!check || gstd_request_parse(check, &validated)) {
        json_decref(check);
        json_decref(intent);
        return GSTD_ACCEPT_INVALID;
    }
    json_decref(check);
    char hash[65];
    char *encoded = canonical(intent);
    if (!encoded || gstd_fingerprint(intent, hash)) {
        free(encoded);
        json_decref(intent);
        return GSTD_ACCEPT_DATABASE;
    }
    const char *deployment = json_string_value(json_object_get(intent, "deploymentId"));
    const char *target = json_string_value(json_object_get(intent, "operationId"));
    const char *profile = json_string_value(json_object_get(intent, "profileId"));
    const char *version = json_string_value(json_object_get(intent, "profileVersion"));
    gstd_accept_status status = GSTD_ACCEPT_DATABASE;
    if (execute(db, "BEGIN IMMEDIATE"))
        goto done;
    sqlite3_stmt *s = prepare(db, "SELECT fingerprint,operation_id FROM dedupe WHERE caller_uid=? "
                                  "AND method=? AND idempotency_key=?");
    if (!s)
        goto rollback;
    sqlite3_bind_int64(s, 1, (sqlite3_int64)geteuid());
    text(s, 2, method);
    text(s, 3, r->idempotency_key);
    int step = sqlite3_step(s);
    if (step == SQLITE_ROW) {
        const char *stored = (const char *)sqlite3_column_text(s, 0),
                   *id = (const char *)sqlite3_column_text(s, 1);
        status = stored && id && !strcmp(stored, hash) ? GSTD_ACCEPT_REPLAY : GSTD_ACCEPT_CONFLICT;
        if (status == GSTD_ACCEPT_REPLAY)
            snprintf(operation, 65, "%s", id);
        sqlite3_finalize(s);
        goto rollback;
    }
    sqlite3_finalize(s);
    if (step != SQLITE_DONE)
        goto rollback;
    s = prepare(db, "SELECT (SELECT count(*) FROM operations),count(*) FROM operations WHERE state "
                    "IN ('accepted','running') AND method!='operation.cancel'");
    if (!s)
        goto rollback;
    step = sqlite3_step(s);
    int full =
        step == SQLITE_ROW && (sqlite3_column_int64(s, 0) >= 100000 ||
                               (r->method != GSTD_CANCEL && sqlite3_column_int64(s, 1) >= 72));
    sqlite3_finalize(s);
    if (step != SQLITE_ROW)
        goto rollback;
    if (full) {
        status = GSTD_ACCEPT_LIMIT;
        goto rollback;
    }
    if (r->method == GSTD_CANCEL) {
        s = prepare(db, "SELECT deployment_id FROM operations WHERE operation_id=?");
        if (!s)
            goto rollback;
        text(s, 1, target);
        step = sqlite3_step(s);
        /* Keep the resolved deployment identity alive after finalizing the query. */
        if (step != SQLITE_ROW) {
            sqlite3_finalize(s);
            status = GSTD_ACCEPT_NOT_FOUND;
            goto rollback;
        }
        const char *found = (const char *)sqlite3_column_text(s, 0);
        if (found && json_object_set_new(intent, "_resolvedDeployment", json_string(found))) {
            sqlite3_finalize(s);
            goto rollback;
        }
        deployment = json_string_value(json_object_get(intent, "_resolvedDeployment"));
        sqlite3_finalize(s);
    } else {
        s = prepare(db, "SELECT 1 FROM operations WHERE deployment_id=? AND state IN "
                        "('accepted','running') AND method!='operation.cancel'");
        if (!s)
            goto rollback;
        text(s, 1, deployment);
        step = sqlite3_step(s);
        sqlite3_finalize(s);
        if (step == SQLITE_ROW) {
            status = GSTD_ACCEPT_DEPLOYMENT_CONFLICT;
            goto rollback;
        }
        if (step != SQLITE_DONE)
            goto rollback;
    }
    sqlite3_int64 generation = 1;
    int existing = 0;
    if (r->method == GSTD_START) {
        s = prepare(
            db,
            "SELECT profile_id,profile_version,generation FROM deployments WHERE deployment_id=?");
        if (!s)
            goto rollback;
        text(s, 1, deployment);
        step = sqlite3_step(s);
        if (step == SQLITE_ROW) {
            existing = 1;
            const char *p = (const char *)sqlite3_column_text(s, 0),
                       *v = (const char *)sqlite3_column_text(s, 1);
            if (!p || !v || strcmp(p, profile) || strcmp(v, version)) {
                sqlite3_finalize(s);
                status = GSTD_ACCEPT_DEPLOYMENT_CONFLICT;
                goto rollback;
            }
            generation = sqlite3_column_int64(s, 2) + 1;
        }
        sqlite3_finalize(s);
        if (step != SQLITE_ROW && step != SQLITE_DONE)
            goto rollback;
        if (generation > GSTD_SAFE_INTEGER) {
            status = GSTD_ACCEPT_LIMIT;
            goto rollback;
        }
        if (volume) {
            s = prepare(db, "SELECT ownership_state FROM resource_ownership WHERE resource_id=?");
            if (!s)
                goto rollback;
            text(s, 1, volume);
            step = sqlite3_step(s);
            int held =
                step == SQLITE_ROW && strcmp((const char *)sqlite3_column_text(s, 0), "released");
            sqlite3_finalize(s);
            if (held) {
                status = GSTD_ACCEPT_OWNERSHIP_UNCERTAIN;
                goto rollback;
            }
            if (step != SQLITE_ROW && step != SQLITE_DONE)
                goto rollback;
        }
    } else if (r->method != GSTD_CANCEL) {
        json_t *expected = json_object_get(r->params, "expectedOperationId");
        if (r->method == GSTD_STOP && expected) {
            s = prepare(db, "SELECT 1 FROM deployments d JOIN operations o ON "
                            "o.operation_id=d.last_operation_id WHERE d.deployment_id=? "
                            "AND d.last_operation_id=? AND o.method='deployment.start'");
            if (!s)
                goto rollback;
            text(s, 1, deployment);
            text(s, 2, json_string_value(expected));
            int matches = sqlite3_step(s) == SQLITE_ROW;
            sqlite3_finalize(s);
            if (!matches) {
                status = GSTD_ACCEPT_DEPLOYMENT_CONFLICT;
                goto rollback;
            }
        }
        s = prepare(db, "SELECT 1 FROM deployments WHERE deployment_id=?");
        if (!s)
            goto rollback;
        text(s, 1, deployment);
        step = sqlite3_step(s);
        sqlite3_finalize(s);
        if (step != SQLITE_ROW) {
            status = GSTD_ACCEPT_NOT_FOUND;
            goto rollback;
        }
    }
    if (operation_identity(operation))
        goto rollback;
    time_t now = time(NULL);
    struct tm utc;
    char timestamp[32];
    if (now < 0 || !gmtime_r(&now, &utc) ||
        !strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc))
        goto rollback;
    if (r->method == GSTD_START) {
        if (!existing) {
            s = prepare(db, "INSERT INTO deployments "
                            "VALUES(?,?,?,'running',?,0,'pending','normal','held',?,?)");
            if (!s)
                goto rollback;
            text(s, 1, deployment);
            text(s, 2, profile);
            text(s, 3, version);
            sqlite3_bind_int64(s, 4, generation);
            text(s, 5, timestamp);
            text(s, 6, operation);
        } else {
            s = prepare(db, "UPDATE deployments SET "
                            "desired_state='running',writer_ownership='held',generation=?,"
                            "readiness='pending',recovery='"
                            "normal',observed_at=?,last_operation_id=? WHERE deployment_id=?");
            if (!s)
                goto rollback;
            sqlite3_bind_int64(s, 1, generation);
            text(s, 2, timestamp);
            text(s, 3, operation);
            text(s, 4, deployment);
        }
        if (finish(s))
            goto rollback;
    }
    s = prepare(db,
                "INSERT INTO operations VALUES(?,?,?,?,?,?,?,?,?,'accepted','normal',NULL,NULL)");
    if (!s)
        goto rollback;
    text(s, 1, operation);
    sqlite3_bind_int64(s, 2, (sqlite3_int64)geteuid());
    text(s, 3, method);
    text(s, 4, deployment);
    text(s, 5, target);
    text(s, 6, hash);
    text(s, 7, encoded);
    text(s, 8, timestamp);
    text(s, 9, timestamp);
    if (finish(s))
        goto rollback;
    s = prepare(db, "INSERT INTO dedupe VALUES(?,?,?,?,?,?)");
    if (!s)
        goto rollback;
    sqlite3_bind_int64(s, 1, (sqlite3_int64)geteuid());
    text(s, 2, method);
    text(s, 3, r->idempotency_key);
    text(s, 4, hash);
    text(s, 5, operation);
    sqlite3_bind_int64(s, 6, (sqlite3_int64)now + 604800);
    if (finish(s))
        goto rollback;
    if (r->method == GSTD_START && volume) {
        s = prepare(db, "INSERT INTO resource_ownership "
                        "VALUES(?,'repository-volume',?,?,'reserved',NULL,'held',?) ON "
                        "CONFLICT(resource_id) DO UPDATE SET "
                        "deployment_id=excluded.deployment_id,operation_id=excluded.operation_id,"
                        "lifecycle_stage='reserved',backend_identity=NULL,ownership_state='held',"
                        "observed_at=excluded.observed_at");
        if (!s)
            goto rollback;
        text(s, 1, volume);
        text(s, 2, deployment);
        text(s, 3, operation);
        text(s, 4, timestamp);
        if (finish(s))
            goto rollback;
    }
    int reserved = gstd_schedule_reserve(db, operation, r->method == GSTD_CANCEL);
    if (reserved) {
        status = reserved > 0 ? GSTD_ACCEPT_LIMIT : GSTD_ACCEPT_DATABASE;
        goto rollback;
    }
    if (execute(db, "COMMIT"))
        goto rollback;
    status = GSTD_ACCEPT_NEW;
    goto done;
rollback:
    if (sqlite3_extended_errcode(db) == SQLITE_FULL)
        status = GSTD_ACCEPT_LIMIT;
    (void)execute(db, "ROLLBACK");
done:
    free(encoded);
    json_decref(intent);
    return status;
}
