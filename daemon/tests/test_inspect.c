#include "gitstored/inspect.h"
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "gitstored/wait.h"
#include "support.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
int main(void) {
    gstd_json_initialize();
    int result = 0;
    char path[256];
    char *root = gstd_test_dir();
    sqlite3 *db = NULL;
    json_t *answer = NULL, *cursor = NULL, *params = NULL;
    CHECK(root);
    CHECK(snprintf(path, sizeof(path), "%s/state.db", root) > 0);
    CHECK(!gstd_state_open(path, &db));
    params = json_pack("{s:i}", "limit", 1);
    CHECK(gstd_inspect(db, GSTD_LIST, params, &answer) == GSTD_QUERY_OK);
    CHECK(json_array_size(json_object_get(answer, "deployments")) == 0);
    json_decref(answer);
    answer = NULL;
    CHECK(
        sqlite3_exec(db,
                     "INSERT INTO profiles VALUES('core','1','rev','digest','linux-arm64');"
                     "INSERT INTO deployments "
                     "VALUES('d1','core','1','running',2,1,'pending','normal','held','2026-11-"
                     "03T00:00:00Z',NULL);"
                     "INSERT INTO deployments "
                     "VALUES('d2','core','1','stopped',1,1,'ready','normal','released','2026-11-"
                     "03T00:00:00Z',NULL);"
                     "INSERT INTO operations "
                     "VALUES('op1',1,'deployment.start','d1',NULL,'"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','{}','2026-"
                     "11-03T00:00:00Z','2026-11-03T00:00:00Z','running','normal',NULL,NULL);",
                     NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_inspect(db, GSTD_LIST, params, &answer) == GSTD_QUERY_OK);
    CHECK(json_array_size(json_object_get(answer, "deployments")) == 1);
    cursor = json_incref(json_object_get(answer, "nextCursor"));
    CHECK(cursor);
    json_decref(answer);
    answer = NULL;
    CHECK(!json_object_set(params, "cursor", cursor));
    CHECK(!sqlite3_close(db));
    db = NULL;
    CHECK(!gstd_state_open(path, &db));
    CHECK(gstd_inspect(db, GSTD_LIST, params, &answer) == GSTD_QUERY_OK);
    CHECK(!strcmp(json_string_value(json_object_get(
                      json_array_get(json_object_get(answer, "deployments"), 0), "deploymentId")),
                  "d2"));
    CHECK(!json_object_get(answer, "nextCursor"));
    json_decref(answer);
    answer = NULL;
    CHECK(sqlite3_exec(
              db,
              "UPDATE deployments SET observed_at='2026-11-03T00:00:01Z' WHERE deployment_id='d1'",
              NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_inspect(db, GSTD_LIST, params, &answer) == GSTD_QUERY_CURSOR_EXPIRED);
    CHECK(!json_object_set_new(params, "cursor", json_string("foreign-cursor")));
    CHECK(gstd_inspect(db, GSTD_LIST, params, &answer) == GSTD_QUERY_CURSOR_EXPIRED);
    json_decref(params);
    params = json_pack("{s:s}", "operationId", "op1");
    CHECK(gstd_wait_poll(db, "op1", "ready", &answer) == GSTD_QUERY_PENDING);
    CHECK(sqlite3_exec(
              db,
              "UPDATE operations SET "
              "state='failed',error='{\"code\":\"OPERATION_FAILED\\u0000synthetic-secret\","
              "\"stage\":\"runtime\","
              "\"requirement\":\"tun\\u0000synthetic-secret\","
              "\"retryable\":false,\"message\":\"synthetic-secret\"}' WHERE operation_id='op1'",
              NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_wait_poll(db, "op1", "ready", &answer) == GSTD_QUERY_OK);
    CHECK(!strcmp(json_string_value(json_object_get(answer, "state")), "failed"));
    char *encoded = json_dumps(answer, JSON_COMPACT);
    CHECK(encoded);
    CHECK(!strstr(encoded, "synthetic-secret"));
    json_malloc_t allocate_json;
    json_free_t free_json;
    json_get_alloc_funcs(&allocate_json, &free_json);
    free_json(encoded);
    json_decref(answer);
    answer = NULL;
    CHECK(gstd_inspect(db, GSTD_OPERATION_INSPECT, params, &answer) == GSTD_QUERY_OK);
    CHECK(!json_object_get(answer, "result"));
    json_decref(answer);
    answer = NULL;
    CHECK(gstd_wait_poll(db, "missing", "terminal", &answer) == GSTD_QUERY_NOT_FOUND);
    CHECK(
        sqlite3_exec(db,
                     "INSERT INTO operations "
                     "VALUES('op2',1,'deployment.stop','d1',NULL,'"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','{}','2026-"
                     "11-03T00:00:00Z','2026-11-03T00:00:00Z','accepted','normal',NULL,NULL);"
                     "INSERT INTO operations "
                     "VALUES('op3',1,'deployment.start','d1',NULL,'"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','{}','2026-"
                     "11-03T00:00:00Z','2026-11-03T00:00:00Z','cancelled','normal',NULL,NULL);"
                     "INSERT INTO operations "
                     "VALUES('op4',1,'deployment.start','d1',NULL,'"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','{}','2026-"
                     "11-03T00:00:00Z','2026-11-03T00:00:00Z','succeeded','normal',NULL,NULL);",
                     NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_wait_poll(db, "op2", "ready", &answer) == GSTD_QUERY_INVALID);
    CHECK(gstd_wait_poll(db, "op3", "ready", &answer) == GSTD_QUERY_OK);
    CHECK(!strcmp(json_string_value(json_object_get(answer, "state")), "cancelled"));
    json_decref(answer);
    answer = NULL;
    CHECK(gstd_wait_poll(db, "op4", "ready", &answer) == GSTD_QUERY_PENDING);
    CHECK(sqlite3_exec(db,
                       "UPDATE deployments SET "
                       "observed_generation=2,readiness='ready',last_operation_id='op4' WHERE "
                       "deployment_id='d1'",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_wait_poll(db, "op4", "ready", &answer) == GSTD_QUERY_OK);
    json_decref(answer);
    answer = NULL;
    CHECK(sqlite3_exec(
              db,
              "UPDATE deployments SET generation=3,readiness='pending' WHERE deployment_id='d1'",
              NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_wait_poll(db, "op4", "ready", &answer) == GSTD_QUERY_PENDING);
    json_decref(params);
    params = json_pack("{s:i}", "limit", 1);
    CHECK(gstd_inspect(db, GSTD_LIST, params, &answer) == GSTD_QUERY_OK);
    CHECK(!json_object_set(params, "cursor", json_object_get(answer, "nextCursor")));
    json_decref(answer);
    answer = NULL;
    CHECK(sqlite3_exec(db, "UPDATE query_cursors SET expires_after=0", NULL, NULL, NULL) ==
          SQLITE_OK);
    CHECK(gstd_inspect(db, GSTD_LIST, params, &answer) == GSTD_QUERY_CURSOR_EXPIRED);
    json_decref(params);
    params = json_pack("{s:i}", "limit", 101);
    CHECK(gstd_inspect(db, GSTD_LIST, params, &answer) == GSTD_QUERY_INVALID);
done:
    json_decref(answer);
    json_decref(cursor);
    json_decref(params);
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
