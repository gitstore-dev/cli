#include "gitstored/operations.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "support.h"
#include <stdio.h>
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
    int result = 0;
    char *root = gstd_test_dir();
    sqlite3 *db = NULL;
    char path[256], first[65] = {0}, id[65];
    json_t *message = NULL, *components = NULL;
    CHECK(root);
    CHECK(snprintf(path, sizeof(path), "%s/jobs.db", root) > 0);
    CHECK(!gstd_state_open(path, &db));
    CHECK(sqlite3_exec(db,
                       "INSERT INTO profiles VALUES('core','v1','source','digest','linux-arm64')",
                       NULL, NULL, NULL) == SQLITE_OK);
    const char *request =
        "{\"version\":{\"major\":1,\"minor\":0},\"requestId\":\"r\",\"method\":\"deployment."
        "start\",\"idempotencyKey\":\"initial\",\"params\":{\"deploymentId\":\"d\",\"profileId\":"
        "\"core\",\"profileVersion\":\"v1\"}}";
    message = gstd_json_parse((const unsigned char *)request, strlen(request));
    CHECK(message);
    components = json_pack("[s]", "api");
    CHECK(components);
    for (unsigned i = 0; i < 73; ++i) {
        char name[32];
        CHECK(snprintf(name, sizeof(name), "deployment-%u", i) > 0);
        json_object_set_new(message, "idempotencyKey", json_string(name));
        json_object_set_new(json_object_get(message, "params"), "deploymentId", json_string(name));
        gstd_request r;
        CHECK(!gstd_request_parse(message, &r));
        CHECK(gstd_accept(db, &r, components, NULL, id) ==
              (i < 72 ? GSTD_ACCEPT_NEW : GSTD_ACCEPT_LIMIT));
        if (!i)
            memcpy(first, id, 65);
    }
    for (unsigned i = 0; i < 8; ++i)
        CHECK(gstd_schedule_take(db, 0, id) == 1);
    CHECK(gstd_schedule_take(db, 0, id) == 0);
    /* Independent cancellation work remains acceptable with all runtime slots occupied. */
    json_object_set_new(message, "method", json_string("operation.cancel"));
    json_object_set_new(message, "idempotencyKey", json_string("cancel"));
    json_object_set_new(message, "params", json_pack("{s:s}", "operationId", first));
    gstd_request cancel;
    CHECK(!gstd_request_parse(message, &cancel));
    CHECK(gstd_accept(db, &cancel, NULL, NULL, id) == GSTD_ACCEPT_NEW);
    /* Saturate the independent pending control queue too. Exhaustion is explicit
     * and cannot erase/reassign any already accepted runtime reservation. */
    for (unsigned i = 1; i <= 64; i++) {
        char key[32];
        CHECK(snprintf(key, sizeof(key), "cancel-%u", i) > 0);
        json_object_set_new(message, "idempotencyKey", json_string(key));
        CHECK(!gstd_request_parse(message, &cancel));
        CHECK(gstd_accept(db, &cancel, NULL, NULL, id) ==
              (i < 64 ? GSTD_ACCEPT_NEW : GSTD_ACCEPT_LIMIT));
    }
    CHECK(gstd_schedule_take(db, 1, id) == 1);
    CHECK(gstd_schedule_finish(db, first) != 0); /* Active work cannot free its own capacity. */
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(
              db, "UPDATE operations SET state='succeeded',result='{}' WHERE operation_id=?", -1,
              &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, first, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    CHECK(!gstd_schedule_finish(db, first));
    CHECK(gstd_schedule_take(db, 0, id) == 1);
done:
    json_decref(message);
    json_decref(components);
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
