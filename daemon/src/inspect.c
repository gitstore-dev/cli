#include "gitstored/inspect.h"
#include "gitstored/assets.h"
#include "migration.h"
#include "oidc_config.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static int prepare(sqlite3 *db, const char *sql, sqlite3_stmt **s) {
    return sqlite3_prepare_v2(db, sql, -1, s, NULL) == SQLITE_OK ? 0 : -1;
}
static int string_column(json_t *object, sqlite3_stmt *s, int column, const char *key) {
    if (sqlite3_column_type(s, column) == SQLITE_NULL)
        return 0;
    const char *text = (const char *)sqlite3_column_text(s, column);
    return !text ? -1
                 : json_object_set_new(object, key,
                                       json_stringn(text, (size_t)sqlite3_column_bytes(s, column)));
}
static int known_error(const char *code) {
    static const char *codes[] = {"INVALID_ARGUMENT",
                                  "NOT_FOUND",
                                  "IDEMPOTENCY_CONFLICT",
                                  "DEPLOYMENT_CONFLICT",
                                  "OWNERSHIP_UNCERTAIN",
                                  "MIGRATION_CONFLICT",
                                  "UNSUPPORTED_CAPABILITY",
                                  "UNSUPPORTED_HOST",
                                  "UNSAFE_CANCELLATION",
                                  "DEADLINE_EXCEEDED",
                                  "RESOURCE_LIMIT",
                                  "CURSOR_EXPIRED",
                                  "STREAM_OVERFLOW",
                                  "PROVISIONING_REQUIRED",
                                  "MIGRATION_PRECONDITION",
                                  "OPERATION_FAILED",
                                  "INTERNAL"};
    for (size_t i = 0; i < sizeof(codes) / sizeof(*codes); i++)
        if (!strcmp(code, codes[i]))
            return 1;
    return 0;
}
static json_t *operation(sqlite3_stmt *s) {
    json_t *record = json_object();
    if (!record)
        return NULL;
    const char *names[] = {"operationId", "method",    "deploymentId", "targetOperationId",
                           "acceptedAt",  "updatedAt", "state",        "recovery"};
    for (int i = 0; i < 8; i++)
        if (string_column(record, s, i, names[i]))
            goto fail;
    /* Result/error persistence is internal. Expose only the stable non-secret
     * identity/state projection; raw backend diagnostics never cross this API. */
    for (int i = 8; i <= 9; i++) {
        if (sqlite3_column_type(s, i) == SQLITE_NULL)
            continue;
        const char *text = (const char *)sqlite3_column_text(s, i);
        json_t *source =
            gstd_json_parse((const unsigned char *)text, (size_t)sqlite3_column_bytes(s, i));
        if (!source)
            goto fail;
        json_t *safe = json_object();
        if (!safe) {
            json_decref(source);
            goto fail;
        }
        const char *result_names[] = {"operationId", "deploymentId", "state",
                                      "readiness",   "generation",   "observedGeneration"};
        const char *error_names[] = {"code", "stage", "retryable"};
        const char **fields = i == 8 ? result_names : error_names;
        size_t count = i == 8 ? sizeof(result_names) / sizeof(*result_names)
                              : sizeof(error_names) / sizeof(*error_names);
        for (size_t j = 0; j < count; j++) {
            json_t *v = json_object_get(source, fields[j]);
            if (i == 9) {
                if (j == 0 &&
                    (!json_is_string(v) || json_string_length(v) != strlen(json_string_value(v)) ||
                     !known_error(json_string_value(v)))) {
                    if (json_object_set_new(safe, "code", json_string("OPERATION_FAILED"))) {
                        json_decref(source);
                        json_decref(safe);
                        goto fail;
                    }
                    continue;
                }
                if (j == 1) {
                    if (json_object_set_new(safe, "stage", json_string("runtime"))) {
                        json_decref(source);
                        json_decref(safe);
                        goto fail;
                    }
                    continue;
                }
                if (j == 2 && !json_is_boolean(v))
                    v = json_false();
            } else if (v) {
                if (json_is_string(v) && json_string_length(v) != strlen(json_string_value(v)))
                    continue;
                if (j < 2 && (!json_is_string(v) ||
                              !gstd_identifier(json_string_value(v), json_string_length(v))))
                    continue;
                if (j >= 4 && (!json_is_integer(v) || json_integer_value(v) < 0 ||
                               json_integer_value(v) > GSTD_SAFE_INTEGER))
                    continue;
                if (j == 2 && (!json_is_string(v) || (strcmp(json_string_value(v), "accepted") &&
                                                      strcmp(json_string_value(v), "running") &&
                                                      strcmp(json_string_value(v), "succeeded") &&
                                                      strcmp(json_string_value(v), "failed") &&
                                                      strcmp(json_string_value(v), "cancelled"))))
                    continue;
                if (j == 3 && (!json_is_string(v) || (strcmp(json_string_value(v), "pending") &&
                                                      strcmp(json_string_value(v), "ready") &&
                                                      strcmp(json_string_value(v), "failed"))))
                    continue;
            }
            if (v && json_object_set(safe, fields[j], v)) {
                json_decref(source);
                json_decref(safe);
                goto fail;
            }
        }
        if (i == 8) {
            const char *method = json_string_value(json_object_get(record, "method")),
                       *state = json_string_value(json_object_get(record, "state"));
            json_t *version = json_object_get(source, "targetVersion"),
                   *digest = json_object_get(source, "targetDigest");
            if (method && !strcmp(method, "migration.start") && state &&
                !strcmp(state, "succeeded") &&
                json_is_true(json_object_get(source, "migrationCompleted")) &&
                json_is_string(version) &&
                json_string_length(version) == strlen(json_string_value(version))) {
                json_t *target = gstd_migration_target(json_string_value(version));
                char expected[65];
                int valid = target && !gstd_fingerprint(target, expected) &&
                            json_is_string(digest) && json_string_length(digest) == 64 &&
                            !memcmp(json_string_value(digest), expected, 64);
                json_decref(target);
                if (valid && (json_object_set(safe, "targetVersion", version) ||
                              json_object_set(safe, "targetDigest", digest) ||
                              json_object_set_new(safe, "migrationCompleted", json_true()))) {
                    json_decref(source);
                    json_decref(safe);
                    goto fail;
                }
            }
            json_t *target_state = json_object_get(source, "targetState");
            if (json_is_string(target_state)) {
                const char *name = json_string_value(target_state);
                if ((!strcmp(name, "accepted") || !strcmp(name, "running") ||
                     !strcmp(name, "succeeded") || !strcmp(name, "failed") ||
                     !strcmp(name, "cancelled")) &&
                    json_string_length(target_state) == strlen(name) &&
                    json_object_set(safe, "targetState", target_state)) {
                    json_decref(source);
                    json_decref(safe);
                    goto fail;
                }
            }
            const char *boolean_fields[] = {"cancelled", "noOp", "stopped", "alreadyStopped"};
            for (unsigned k = 0; k < 4; k++) {
                json_t *v = json_object_get(source, boolean_fields[k]);
                if (json_is_boolean(v) && json_object_set(safe, boolean_fields[k], v)) {
                    json_decref(source);
                    json_decref(safe);
                    goto fail;
                }
            }
        }
        if (i == 9) {
            json_t *requirement = json_object_get(source, "requirement");
            static const char *names[] = {"linux-kernel",      "user-session", "cgroup-v2",
                                          "cgroup-delegation", "user-bus",     "tun",
                                          "user-namespaces",   "pidfd",        "openat2",
                                          "runtime-library"};
            for (unsigned n = 0; n < sizeof(names) / sizeof(*names); n++) {
                if (json_is_string(requirement) &&
                    json_string_length(requirement) == strlen(names[n]) &&
                    !strcmp(json_string_value(requirement), names[n]) &&
                    json_object_set(safe, "requirement", requirement)) {
                    json_decref(source);
                    json_decref(safe);
                    goto fail;
                }
            }
        }
        if (i == 9 && json_object_set_new(
                          safe, "message",
                          json_string("Operation failed; raw backend diagnostics are withheld"))) {
            json_decref(source);
            json_decref(safe);
            goto fail;
        }
        json_decref(source);
        if (json_object_set_new(record, i == 8 ? "result" : "error", safe))
            goto fail;
    }
    return record;
fail:
    json_decref(record);
    return NULL;
}
static unsigned endpoint_port(const json_t *ports, const char *name) {
    const json_t *value = json_object_get(ports, name);
    json_int_t port = json_integer_value(value);
    return json_is_integer(value) && port >= 1024 && port <= 65535 ? (unsigned)port : 0;
}
static json_t *endpoint_urls(const json_t *ports, int oidc_profile) {
    const char *host = json_string_value(json_object_get(ports, "host")),
               *git_host = json_string_value(json_object_get(ports, "gitHost"));
    int core = json_object_get(ports, "apiPort") || json_object_get(ports, "gitPort");
    int oidc = json_object_get(ports, "oidcIssuerPort") ||
               json_object_get(ports, "oidcBridgePort") || json_object_get(ports, "identityPort") ||
               json_object_get(ports, "identityUiPort") || json_object_get(ports, "mailPort");
    if (!host || json_string_length(json_object_get(ports, "host")) != 9 ||
        strcmp(host, "127.0.0.1") || (!core && !oidc) ||
        json_object_size(ports) != (core ? 4u : 1u) + (oidc ? 5u : 0u) || (oidc && !oidc_profile))
        return NULL;
    unsigned api = endpoint_port(ports, "apiPort"), git = endpoint_port(ports, "gitPort");
    gstd_oidc_ports selected = {(uint16_t)endpoint_port(ports, "oidcIssuerPort"),
                                (uint16_t)endpoint_port(ports, "oidcBridgePort"),
                                (uint16_t)endpoint_port(ports, "identityPort"),
                                (uint16_t)endpoint_port(ports, "identityUiPort"),
                                (uint16_t)endpoint_port(ports, "mailPort")};
    if ((core && (!api || !git || api == git || !git_host ||
                  json_string_length(json_object_get(ports, "gitHost")) != 9 ||
                  strcmp(git_host, "127.0.0.1"))) ||
        (oidc && gstd_oidc_ports_check(&selected)))
        return NULL;
    const unsigned values[] = {
        api, git, selected.issuer, selected.bridge, selected.identity, selected.ui, selected.mail};
    for (unsigned i = 0; i < 7; i++)
        for (unsigned j = 0; values[i] && j < i; j++)
            if (values[i] == values[j])
                return NULL;
    const char *names[] = {"api",      "git",        "oidcIssuer", "oidcBridge",
                           "identity", "identityUi", "mail"};
    json_t *urls = json_object();
    for (unsigned i = 0; urls && i < 7; i++) {
        if (!values[i])
            continue;
        char url[64];
        snprintf(url, sizeof(url), "http://127.0.0.1:%u%s", values[i], i == 2 ? "/" : "");
        if (json_object_set_new(urls, names[i], json_string(url))) {
            json_decref(urls);
            return NULL;
        }
    }
    return urls;
}
static json_t *deployment(sqlite3 *db, sqlite3_stmt *s) {
    json_t *record = json_object();
    sqlite3_stmt *children = NULL;
    if (!record)
        return NULL;
    const char *names[] = {"deploymentId",    "profileId",  "profileVersion",
                           "desiredState",    "readiness",  "recovery",
                           "writerOwnership", "observedAt", "lastOperationId"};
    for (int i = 0; i < 9; i++)
        if (string_column(record, s, i, names[i]))
            goto fail;
    if (json_object_set_new(record, "generation", json_integer(sqlite3_column_int64(s, 9))) ||
        json_object_set_new(record, "observedGeneration",
                            json_integer(sqlite3_column_int64(s, 10))) ||
        json_object_set_new(record, "components", json_array()) ||
        json_object_set_new(record, "resources", json_array()))
        goto fail;
    const char *id = (const char *)sqlite3_column_text(s, 0);
    if (prepare(db,
                "SELECT component,backend_id,desired_state,observed_state,observed_at,generation "
                "FROM components WHERE deployment_id=? ORDER BY component",
                &children))
        goto fail;
    sqlite3_bind_text(children, 1, id, -1, SQLITE_TRANSIENT);
    int code;
    while ((code = sqlite3_step(children)) == SQLITE_ROW) {
        json_t *child = json_object();
        if (!child)
            goto fail;
        const char *fields[] = {"component", "backendId", "desiredState", "observedState",
                                "observedAt"};
        for (int i = 0; i < 5; i++)
            if (string_column(child, children, i, fields[i])) {
                json_decref(child);
                goto fail;
            }
        if (json_object_set_new(child, "generation",
                                json_integer(sqlite3_column_int64(children, 5))) ||
            json_array_append_new(json_object_get(record, "components"), child))
            goto fail;
    }
    sqlite3_finalize(children);
    children = NULL;
    if (code != SQLITE_DONE)
        goto fail;
    if (prepare(db,
                "SELECT resource_id,resource_type,ownership_state,lifecycle_stage,observed_at FROM "
                "resource_ownership WHERE deployment_id=? ORDER BY resource_id LIMIT 4097",
                &children))
        goto fail;
    sqlite3_bind_text(children, 1, id, -1, SQLITE_TRANSIENT);
    while ((code = sqlite3_step(children)) == SQLITE_ROW) {
        if (json_array_size(json_object_get(record, "resources")) == 4096)
            goto fail;
        json_t *child = json_object();
        if (!child)
            goto fail;
        const char *fields[] = {"resourceId", "resourceType", "ownershipState", "lifecycleStage",
                                "observedAt"};
        for (int i = 0; i < 5; i++)
            if (string_column(child, children, i, fields[i])) {
                json_decref(child);
                goto fail;
            }
        if (json_array_append_new(json_object_get(record, "resources"), child))
            goto fail;
    }
    sqlite3_finalize(children);
    if (code != SQLITE_DONE) {
        json_decref(record);
        return NULL;
    }
    if (json_equal(json_object_get(record, "generation"),
                   json_object_get(record, "observedGeneration")) &&
        !strcmp(json_string_value(json_object_get(record, "readiness")), "ready") &&
        !strcmp(json_string_value(json_object_get(record, "recovery")), "normal")) {
        sqlite3_stmt *endpoint = NULL;
        if (prepare(db,
                    "SELECT result FROM operations WHERE operation_id=? AND state='succeeded' "
                    "AND method='deployment.start'",
                    &endpoint))
            goto fail;
        const char *operation_id = json_string_value(json_object_get(record, "lastOperationId"));
        sqlite3_bind_text(endpoint, 1, operation_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(endpoint) == SQLITE_ROW) {
            const unsigned char *raw = sqlite3_column_text(endpoint, 0);
            json_t *result =
                raw ? gstd_json_parse(raw, (size_t)sqlite3_column_bytes(endpoint, 0)) : NULL;
            json_t *ports = json_object_get(result, "endpoints");
            const char *version = json_string_value(json_object_get(record, "profileVersion"));
            json_t *urls = json_is_integer(json_object_get(result, "generation")) &&
                json_equal(json_object_get(result, "generation"), json_object_get(record, "generation"))
                ? endpoint_urls(ports, version && !strcmp(version, "2")) : NULL;
            if (urls && json_object_set_new(record, "endpoints", urls)) {
                json_decref(result);
                sqlite3_finalize(endpoint);
                goto fail;
            }
            json_decref(result);
        }
        sqlite3_finalize(endpoint);
    }
    return record;
fail:
    sqlite3_finalize(children);
    json_decref(record);
    return NULL;
}
static const char *deployment_sql =
    "SELECT "
    "deployment_id,profile_id,profile_version,desired_state,readiness,recovery,writer_ownership,"
    "observed_at,last_operation_id,generation,observed_generation FROM deployments";
static gstd_query_status list(sqlite3 *db, json_t *params, json_t **out) {
    sqlite3_stmt *s = NULL;
    gstd_query_status status = GSTD_QUERY_DATABASE;
    json_t *result = NULL;
    char last[65] = "", store[33];
    sqlite3_int64 generation = 0;
    json_t *bound = json_object_get(params, "limit"), *cursor = json_object_get(params, "cursor");
    sqlite3_int64 limit = bound ? json_integer_value(bound) : 100;
    if ((bound && !json_is_integer(bound)) || limit < 1 || limit > 100 ||
        (cursor && (!json_is_string(cursor) || !json_string_length(cursor))))
        return GSTD_QUERY_INVALID;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return status;
    if (prepare(db, "SELECT store_id,generation FROM query_meta", &s) ||
        sqlite3_step(s) != SQLITE_ROW)
        goto done;
    const char *identity = (const char *)sqlite3_column_text(s, 0);
    if (!identity || strlen(identity) != 32)
        goto done;
    memcpy(store, identity, 33);
    generation = sqlite3_column_int64(s, 1);
    sqlite3_finalize(s);
    s = NULL;
    if (cursor) {
        if (prepare(db,
                    "SELECT last_id FROM query_cursors WHERE cursor_id=? AND store_id=? AND "
                    "generation=? AND expires_after>?",
                    &s))
            goto done;
        sqlite3_bind_text(s, 1, json_string_value(cursor), (int)json_string_length(cursor),
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, store, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 3, generation);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)time(NULL));
        if (sqlite3_step(s) != SQLITE_ROW) {
            status = GSTD_QUERY_CURSOR_EXPIRED;
            goto done;
        }
        const char *position = (const char *)sqlite3_column_text(s, 0);
        if (!position || !gstd_identifier(position, (size_t)sqlite3_column_bytes(s, 0)))
            goto done;
        memcpy(last, position, (size_t)sqlite3_column_bytes(s, 0) + 1);
        sqlite3_finalize(s);
        s = NULL;
    }
    char sql[512];
    int n = snprintf(sql, sizeof(sql), "%s WHERE deployment_id>? ORDER BY deployment_id LIMIT ?",
                     deployment_sql);
    if (n < 0 || (size_t)n >= sizeof(sql) || prepare(db, sql, &s))
        goto done;
    sqlite3_bind_text(s, 1, last, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, limit + 1);
    result = json_pack("{s:[]}", "deployments");
    if (!result)
        goto done;
    int code;
    int more = 0;
    while ((code = sqlite3_step(s)) == SQLITE_ROW) {
        if (json_array_size(json_object_get(result, "deployments")) == (size_t)limit) {
            more = 1;
            break;
        }
        json_t *item = deployment(db, s);
        if (!item)
            goto done;
        const char *id = json_string_value(json_object_get(item, "deploymentId"));
        if (!id || !gstd_identifier(id, strlen(id))) {
            json_decref(item);
            goto done;
        }
        memcpy(last, id, strlen(id) + 1);
        if (json_array_append_new(json_object_get(result, "deployments"), item))
            goto done;
    }
    sqlite3_finalize(s);
    s = NULL;
    if (code != SQLITE_DONE && !more)
        goto done;
    if (more) {
        if (prepare(db, "DELETE FROM query_cursors WHERE expires_after<=?", &s))
            goto done;
        sqlite3_bind_int64(s, 1, (sqlite3_int64)time(NULL));
        if (sqlite3_step(s) != SQLITE_DONE)
            goto done;
        sqlite3_finalize(s);
        s = NULL;
        if (prepare(db, "SELECT count(*) FROM query_cursors", &s) || sqlite3_step(s) != SQLITE_ROW)
            goto done;
        if (sqlite3_column_int64(s, 0) >= 4096) {
            status = GSTD_QUERY_LIMIT;
            goto done;
        }
        sqlite3_finalize(s);
        s = NULL;
        if (prepare(db,
                    "INSERT INTO query_cursors VALUES(lower(hex(randomblob(16))),?,?,?,?) "
                    "RETURNING cursor_id",
                    &s))
            goto done;
        sqlite3_bind_text(s, 1, store, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, generation);
        sqlite3_bind_text(s, 3, last, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 4, (sqlite3_int64)time(NULL) + 300);
        if (sqlite3_step(s) != SQLITE_ROW || string_column(result, s, 0, "nextCursor"))
            goto done;
        if (sqlite3_step(s) != SQLITE_DONE)
            goto done;
        sqlite3_finalize(s);
        s = NULL;
    }
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
        goto done;
    *out = result;
    result = NULL;
    status = GSTD_QUERY_OK;
done:
    sqlite3_finalize(s);
    json_decref(result);
    if (status != GSTD_QUERY_OK)
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return status;
}
gstd_query_status gstd_inspect(sqlite3 *db, gstd_method method, json_t *params, json_t **out) {
    *out = NULL;
    if (!db || !json_is_object(params))
        return GSTD_QUERY_INVALID;
    if (method == GSTD_LIST)
        return list(db, params, out);
    if (method != GSTD_INSPECT && method != GSTD_OPERATION_INSPECT)
        return GSTD_QUERY_INVALID;
    json_t *value =
        json_object_get(params, method == GSTD_INSPECT ? "deploymentId" : "operationId");
    if (!json_is_string(value) ||
        !gstd_identifier(json_string_value(value), json_string_length(value)))
        return GSTD_QUERY_INVALID;
    const char *operation_sql =
        "SELECT "
        "operation_id,method,deployment_id,target_operation_id,accepted_at,updated_at,state,"
        "recovery,result,error FROM operations WHERE operation_id=?";
    char sql[512];
    int n = snprintf(sql, sizeof(sql), "%s WHERE deployment_id=?", deployment_sql);
    if (n < 0 || (size_t)n >= sizeof(sql))
        return GSTD_QUERY_DATABASE;
    sqlite3_stmt *s = NULL;
    if (prepare(db, method == GSTD_INSPECT ? sql : operation_sql, &s))
        return GSTD_QUERY_DATABASE;
    sqlite3_bind_text(s, 1, json_string_value(value), (int)json_string_length(value),
                      SQLITE_TRANSIENT);
    int code = sqlite3_step(s);
    gstd_query_status status = GSTD_QUERY_DATABASE;
    if (code == SQLITE_DONE)
        status = GSTD_QUERY_NOT_FOUND;
    else if (code == SQLITE_ROW) {
        *out = method == GSTD_INSPECT ? deployment(db, s) : operation(s);
        if (*out)
            status = GSTD_QUERY_OK;
    }
    sqlite3_finalize(s);
    return status;
}
