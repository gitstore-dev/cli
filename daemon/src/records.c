#include "gitstored/records.h"
#include "datastore_schema.inc"
#include "gitstored/protocol.h"
#include "migration_network_schema.inc"
#include "observer_schema.inc"
#include "oidc_volume_schema.inc"
#include "schema.inc"
#include <string.h>
static void identifier_check(sqlite3_context *context, int argc, sqlite3_value **values) {
    (void)argc;
    if (sqlite3_value_type(values[0]) != SQLITE_TEXT) {
        sqlite3_result_int(context, 0);
        return;
    }
    const unsigned char *text = sqlite3_value_text(values[0]);
    int bytes = sqlite3_value_bytes(values[0]);
    sqlite3_result_int(context, text && gstd_identifier((const char *)text, (size_t)bytes));
}
static void migration_preflight(sqlite3_context *context, int argc, sqlite3_value **values) {
    (void)argc;
    int valid = sqlite3_value_type(values[0]) == SQLITE_TEXT &&
                sqlite3_value_type(values[1]) == SQLITE_INTEGER &&
                sqlite3_value_int64(values[1]) > 0 &&
                sqlite3_value_int64(values[1]) <= GSTD_SAFE_INTEGER &&
                sqlite3_value_bytes(values[0]) > 0 && sqlite3_value_bytes(values[0]) <= 32768;
    json_t *value = valid ? gstd_json_parse(sqlite3_value_text(values[0]),
                                            (size_t)sqlite3_value_bytes(values[0]))
                          : NULL;
    json_t *kind = json_object_get(value, "kind"), *stage = json_object_get(value, "stage");
    valid = value && json_is_object(value) && json_object_size(value) == 7 &&
            json_is_string(kind) && json_string_length(kind) == 17 &&
            !memcmp(json_string_value(kind), "migration-control", 17) && json_is_string(stage) &&
            json_string_length(stage) == 9 && !memcmp(json_string_value(stage), "preflight", 9) &&
            json_is_integer(json_object_get(value, "generation")) &&
            json_integer_value(json_object_get(value, "generation")) ==
                sqlite3_value_int64(values[1]) &&
            json_is_false(json_object_get(value, "unsafe"));
    json_decref(value);
    sqlite3_result_int(context, valid);
}
int gstd_records_functions(sqlite3 *db) {
    return sqlite3_create_function_v2(db, "gstd_identifier", 1,
                                      SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, NULL,
                                      identifier_check, NULL, NULL, NULL) == SQLITE_OK &&
                   sqlite3_create_function_v2(db, "gstd_migration_preflight", 2,
                                              SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
                                              NULL, migration_preflight, NULL, NULL,
                                              NULL) == SQLITE_OK
               ? 0
               : -1;
}
int gstd_records_create(sqlite3 *db) {
    return sqlite3_exec(db, schema_sql, NULL, NULL, NULL) == SQLITE_OK &&
                   sqlite3_exec(db, observer_schema_sql, NULL, NULL, NULL) == SQLITE_OK &&
                   sqlite3_exec(db, datastore_schema_sql, NULL, NULL, NULL) == SQLITE_OK &&
                   sqlite3_exec(db, migration_network_schema_sql, NULL, NULL, NULL) == SQLITE_OK &&
                   sqlite3_exec(db, oidc_volume_schema_sql, NULL, NULL, NULL) == SQLITE_OK
               ? 0
               : -1;
}
static int schema_matches(sqlite3 *db, int version) {
    /* Required definitions use the same maintained SQLite parser. Columns alone
     * cannot prove ownership constraints and immutable-receipt triggers remain. */
    sqlite3 *expected = NULL;
    sqlite3_stmt *objects = NULL, *actual = NULL, *unknown = NULL;
    int valid = 0;
    if (sqlite3_open_v2(":memory:", &expected,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY,
                        NULL) != SQLITE_OK ||
        gstd_records_functions(expected) ||
        sqlite3_exec(expected, schema_sql, NULL, NULL, NULL) != SQLITE_OK ||
        (version >= 2 &&
         sqlite3_exec(expected, observer_schema_sql, NULL, NULL, NULL) != SQLITE_OK) ||
        (version >= 3 &&
         sqlite3_exec(expected, datastore_schema_sql, NULL, NULL, NULL) != SQLITE_OK) ||
        (version >= 4 &&
         sqlite3_exec(expected, migration_network_schema_sql, NULL, NULL, NULL) != SQLITE_OK) ||
        (version >= 5 &&
         sqlite3_exec(expected, oidc_volume_schema_sql, NULL, NULL, NULL) != SQLITE_OK) ||
        sqlite3_prepare_v2(expected, "SELECT type,name,sql FROM sqlite_schema ORDER BY name", -1,
                           &objects, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT type,sql FROM sqlite_schema WHERE name=?", -1, &actual,
                           NULL) != SQLITE_OK)
        goto done;
    int step;
    while ((step = sqlite3_step(objects)) == SQLITE_ROW) {
        sqlite3_reset(actual);
        sqlite3_clear_bindings(actual);
        sqlite3_bind_text(actual, 1, (const char *)sqlite3_column_text(objects, 1), -1,
                          SQLITE_TRANSIENT);
        if (sqlite3_step(actual) != SQLITE_ROW)
            goto done;
        const char *type = (const char *)sqlite3_column_text(objects, 0);
        if (!type || sqlite3_column_bytes(actual, 0) != sqlite3_column_bytes(objects, 0) ||
            !sqlite3_column_text(actual, 0) ||
            memcmp(sqlite3_column_text(actual, 0), type, (size_t)sqlite3_column_bytes(objects, 0)))
            goto done;
        int want = sqlite3_column_type(objects, 2), have = sqlite3_column_type(actual, 1);
        if (want != have ||
            (want != SQLITE_NULL &&
             (sqlite3_column_bytes(objects, 2) != sqlite3_column_bytes(actual, 1) ||
              !sqlite3_column_text(actual, 1) ||
              memcmp(sqlite3_column_text(objects, 2), sqlite3_column_text(actual, 1),
                     (size_t)sqlite3_column_bytes(objects, 2)))))
            goto done;
    }
    if (step != SQLITE_DONE ||
        sqlite3_prepare_v2(db, "SELECT name FROM sqlite_schema WHERE type='trigger'", -1, &unknown,
                           NULL) != SQLITE_OK)
        goto done;
    sqlite3_finalize(actual);
    actual = NULL;
    if (sqlite3_prepare_v2(expected, "SELECT 1 FROM sqlite_schema WHERE type='trigger' AND name=?",
                           -1, &actual, NULL) != SQLITE_OK)
        goto done;
    while ((step = sqlite3_step(unknown)) == SQLITE_ROW) {
        sqlite3_reset(actual);
        sqlite3_clear_bindings(actual);
        sqlite3_bind_text(actual, 1, (const char *)sqlite3_column_text(unknown, 0), -1,
                          SQLITE_TRANSIENT);
        if (sqlite3_step(actual) != SQLITE_ROW)
            goto done;
    }
    valid = step == SQLITE_DONE;
done:
    sqlite3_finalize(unknown);
    sqlite3_finalize(actual);
    sqlite3_finalize(objects);
    sqlite3_close(expected);
    return valid ? 0 : -1;
}
int gstd_records_check(sqlite3 *db) {
    if (schema_matches(db, 5))
        return -1;
    sqlite3_stmt *observer = NULL;
    if (sqlite3_prepare_v2(
            db, "SELECT worker_slot,subscription_id,deployment_id FROM observer_slots LIMIT 0", -1,
            &observer, NULL) != SQLITE_OK)
        return -1;
    int observer_valid = sqlite3_step(observer) == SQLITE_DONE;
    sqlite3_finalize(observer);
    if (!observer_valid)
        return -1;
    /* Refer to every required column, refusing incompatible stores rather than repairing them. */
    static const char *queries[] = {
        "SELECT "
        "asset_key,descriptor,producer_operation_id,operation_id,staging_name,staging_device,"
        "staging_inode,state,reserved_bytes,download_limit,merge_limit,tree_digest,logical_bytes,"
        "downloaded_bytes,merged_"
        "bytes FROM assets LIMIT 0",
        "SELECT resource_id,asset_key FROM asset_refs LIMIT 0",
        "SELECT store_id,generation FROM query_meta LIMIT 0",
        "SELECT cursor_id,store_id,generation,last_id,expires_after FROM query_cursors LIMIT 0",
        "SELECT operation_id,kind,state,worker_slot FROM jobs LIMIT 0",
        "SELECT profile_id,profile_version,server_revision,manifest_digest,platform FROM profiles "
        "LIMIT 0",
        "SELECT "
        "deployment_id,profile_id,profile_version,desired_state,generation,observed_generation,"
        "readiness,recovery,writer_ownership,observed_at,last_operation_id FROM deployments LIMIT "
        "0",
        "SELECT "
        "operation_id,caller_uid,method,deployment_id,target_operation_id,intent_fingerprint,"
        "intent,accepted_at,updated_at,state,recovery,result,error FROM operations LIMIT 0",
        "SELECT caller_uid,method,idempotency_key,fingerprint,operation_id,expires_after FROM "
        "dedupe LIMIT 0",
        "SELECT "
        "deployment_id,component,backend_id,image_digest,desired_state,observed_state,generation,"
        "observed_at FROM components LIMIT 0",
        "SELECT "
        "resource_id,resource_type,deployment_id,operation_id,lifecycle_stage,backend_identity,"
        "ownership_state,observed_at FROM resource_ownership LIMIT 0"};
    for (size_t i = 0; i < sizeof(queries) / sizeof(*queries); ++i) {
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(db, queries[i], -1, &s, NULL) != SQLITE_OK)
            return -1;
        int result = sqlite3_step(s);
        sqlite3_finalize(s);
        if (result != SQLITE_DONE)
            return -1;
    }
    sqlite3_stmt *meta = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT count(*) FROM query_meta WHERE length(store_id)=32 AND store_id "
                           "NOT GLOB '*[^0-9a-f]*' AND generation BETWEEN 1 AND 9007199254740991",
                           -1, &meta, NULL) != SQLITE_OK)
        return -1;
    int valid = sqlite3_step(meta) == SQLITE_ROW && sqlite3_column_int64(meta, 0) == 1;
    sqlite3_finalize(meta);
    if (!valid)
        return -1;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM query_meta", -1, &meta, NULL) != SQLITE_OK)
        return -1;
    valid = sqlite3_step(meta) == SQLITE_ROW && sqlite3_column_int64(meta, 0) == 1;
    sqlite3_finalize(meta);
    return valid ? 0 : -1;
}

int gstd_records_upgrade_v1(sqlite3 *db) {
    if (!db || sqlite3_get_autocommit(db) || schema_matches(db, 1))
        return -1;
    return sqlite3_exec(db, observer_schema_sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

int gstd_records_upgrade_v2(sqlite3 *db) {
    if (!db || sqlite3_get_autocommit(db) || schema_matches(db, 2))
        return -1;
    return sqlite3_exec(db, datastore_schema_sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

int gstd_records_upgrade_v3(sqlite3 *db) {
    if (!db || sqlite3_get_autocommit(db) || schema_matches(db, 3))
        return -1;
    return sqlite3_exec(db, migration_network_schema_sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

int gstd_records_upgrade_v4(sqlite3 *db) {
    if (!db || sqlite3_get_autocommit(db) || schema_matches(db, 4))
        return -1;
    return sqlite3_exec(db, oidc_volume_schema_sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
