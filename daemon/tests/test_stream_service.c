#define _POSIX_C_SOURCE 200809L
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "logs.h"
#include "stream_service.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "stream service line %d: %s\n", __LINE__, #x);                         \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int text(const json_t *value, const char *key, const char *expected) {
    const char *v = json_string_value(json_object_get(value, key));
    return v && !strcmp(v, expected);
}
static int reply(gstd_stream_service *s, unsigned connection, json_t **value, int *error) {
    for (unsigned i = 0; i < 6000; i++) {
        gstd_stream_service_poll(s);
        int ready = gstd_stream_service_reply(s, connection, value, error);
        if (ready)
            return ready;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    return -1;
}
int main(void) {
    gstd_json_initialize();
    int result = 1, state = -1, directory = -1, error = 0;
    char *root = gstd_test_dir(), path[512], store[33], fast[65], slow[65];
    sqlite3 *db = NULL;
    gstd_logs *logs = NULL;
    gstd_stream_service *s = NULL;
    json_t *value = NULL,
           *params = json_pack("{s:s,s:s}", "deploymentId", "observed", "kind", "logs"),
           *payload = json_pack("{s:s}", "text", "redacted fixture");
    CHECK(root && params && payload);
    snprintf(path, sizeof(path), "%s/state.db", root);
    CHECK(!gstd_state_open(path, &db));
    CHECK(sqlite3_exec(
              db,
              "INSERT INTO profiles VALUES('core','1','source','digest','linux-arm64');"
              "INSERT INTO deployments "
              "VALUES('observed','core','1','running',1,0,'pending','normal','released','2000-01-"
              "01T00:00:00Z',NULL);"
              "INSERT INTO components "
              "VALUES('observed','api',NULL,'digest','running','pending',1,'2000-01-01T00:00:00Z')",
              NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_stmt *q = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT store_id FROM query_meta", -1, &q, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(q) == SQLITE_ROW);
    snprintf(store, sizeof(store), "%s", sqlite3_column_text(q, 0));
    sqlite3_finalize(q);
    state = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(state >= 3);
    directory = gstd_logs_directory(state, store, 1);
    CHECK(directory >= 3 && gstd_logs_open_at(directory, NULL, 1, &logs) == GSTD_LOG_OK);
    const char *components[] = {"api", "scylladb", "controller"};
    for (unsigned i = 0; i < 300; i++)
        CHECK(gstd_logs_append(logs, "observed", components[i % 3], GSTD_STREAM_LOGS, payload,
                               (int64_t)time(NULL)) == GSTD_LOG_OK);
    s = gstd_stream_service_open(db, state);
    CHECK(s);
    CHECK(!json_object_set_new(
        params, "cursor",
        json_string("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")));
    CHECK(gstd_stream_service_subscribe(s, 0, params, &value, &error) == 0 && error &&
          text(value, "code", "INVALID_ARGUMENT"));
    json_decref(value);
    value = NULL;
    json_object_del(params, "cursor");
    CHECK(gstd_stream_service_subscribe(s, 0, params, &value, &error) == 1 && !value);
    CHECK(gstd_stream_service_subscribe(s, 1, params, &value, &error) == 1 && !value);
    CHECK(gstd_stream_service_subscribe(s, 2, params, &value, &error) == 0 && error &&
          text(value, "code", "RESOURCE_LIMIT"));
    json_decref(value);
    value = NULL;
    CHECK(reply(s, 0, &value, &error) == 1 && !error);
    snprintf(fast, sizeof(fast), "%s", json_string_value(json_object_get(value, "subscriptionId")));
    json_decref(value);
    value = NULL;
    CHECK(reply(s, 1, &value, &error) == 1 && !error);
    snprintf(slow, sizeof(slow), "%s", json_string_value(json_object_get(value, "subscriptionId")));
    json_decref(value);
    value = NULL;
    unsigned received = 0;
    for (unsigned i = 0; i < 8000 && received < 300; i++) {
        gstd_stream_service_poll(s);
        while (gstd_stream_service_next(s, 0, &value) == 1) {
            CHECK(text(value, "subscriptionId", fast) && text(value, "type", "log"));
            CHECK(json_integer_value(json_object_get(value, "sequence")) == ++received);
            CHECK(text(json_object_get(value, "payload"), "component",
                       components[(received - 1) % 3]));
            json_decref(value);
            value = NULL;
        }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    CHECK(received == 300);
    CHECK(gstd_stream_service_next(s, 1, &value) == 1 && text(value, "subscriptionId", slow) &&
          text(value, "type", "gap"));
    CHECK(text(json_object_get(value, "payload"), "code", "STREAM_OVERFLOW"));
    json_decref(value);
    value = NULL;
    value = gstd_stream_service_unsubscribe(s, 0, slow, &error);
    CHECK(value && error && text(value, "code", "NOT_FOUND"));
    json_decref(value);
    value = NULL;
    value = gstd_stream_service_unsubscribe(s, 0, fast, &error);
    CHECK(value && !error && json_is_true(json_object_get(value, "closed")));
    json_decref(value);
    value = NULL;
    value = gstd_stream_service_unsubscribe(s, 0, fast, &error);
    CHECK(value && !error && json_is_false(json_object_get(value, "closed")));
    json_decref(value);
    value = NULL;
    CHECK(!json_object_set_new(params, "cursor", json_string("log:expired:1")));
    CHECK(gstd_stream_service_subscribe(s, 2, params, &value, &error) == 1);
    CHECK(reply(s, 2, &value, &error) == 1 && error && text(value, "code", "CURSOR_EXPIRED"));
    json_decref(value);
    value = NULL;
    json_object_del(params, "cursor");
    CHECK(gstd_stream_service_subscribe(s, 3, params, &value, &error) == 1);
    gstd_stream_service_disconnect(s, 3);
    for (unsigned i = 0; i < 100; i++) {
        gstd_stream_service_poll(s);
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    CHECK(gstd_stream_service_reply(s, 3, &value, &error) == 0 && !value);
    CHECK(gstd_stream_service_next(s, 3, &value) == 0 && !value);
    CHECK(!json_object_set_new(params, "component", json_string("oidc")));
    CHECK(gstd_stream_service_subscribe(s, 4, params, &value, &error) == 0 && error &&
          text(value, "code", "NOT_FOUND"));
    json_decref(value);
    value = NULL;
    CHECK(sqlite3_exec(db,
                       "INSERT INTO components VALUES('observed','oidc-bridge',NULL,"
                       "'digest','running','running',1,'2000-01-01T00:00:00Z')",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(gstd_logs_append(logs, "observed", "oidc-bridge", GSTD_STREAM_LOGS, payload,
                           (int64_t)time(NULL)) == GSTD_LOG_OK);
    CHECK(gstd_stream_service_subscribe(s, 4, params, &value, &error) == 1 && !value);
    CHECK(reply(s, 4, &value, &error) == 1 && !error);
    json_decref(value);
    value = NULL;
    CHECK(gstd_stream_service_next(s, 4, &value) == 1 && text(value, "type", "log") &&
          text(json_object_get(value, "payload"), "component", "oidc-bridge"));
    json_decref(value);
    value = NULL;
    gstd_stream_service_disconnect(s, 4);
    result = 0;
done:
    json_decref(value);
    json_decref(params);
    json_decref(payload);
    gstd_stream_service_close(s);
    gstd_logs_close(logs);
    if (directory >= 0)
        close(directory);
    if (state >= 0)
        close(state);
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
