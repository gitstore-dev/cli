#include "gitstored/operations.h"
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
    char path[256], first[65], second[65];
    json_t *message = NULL, *components = NULL;
    CHECK(root);
    CHECK(snprintf(path, sizeof(path), "%s/accept.db", root) > 0);
    CHECK(!gstd_state_open(path, &db));
    CHECK(sqlite3_exec(db,
                       "INSERT INTO profiles VALUES('core','v1','source','digest','linux-arm64')",
                       NULL, NULL, NULL) == SQLITE_OK);
    const char *request =
        "{\"version\":{\"major\":1,\"minor\":0},\"requestId\":\"r1\",\"method\":\"deployment."
        "start\",\"idempotencyKey\":\"same\",\"params\":{\"deploymentId\":\"d1\",\"profileId\":"
        "\"core\",\"profileVersion\":\"v1\"}}";
    message = gstd_json_parse((const unsigned char *)request, strlen(request));
    CHECK(message);
    components = json_pack("[s,s,s]", "api", "git", "controller");
    CHECK(components);
    json_t *empty = json_object();
    char fingerprint[65];
    CHECK(!gstd_fingerprint(empty, fingerprint));
    json_decref(empty);
    CHECK(!strcmp(fingerprint, "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"));
    gstd_request r;
    CHECK(!gstd_request_parse(message, &r));
    CHECK(gstd_accept(db, &r, components, "repo-volume", first) == GSTD_ACCEPT_NEW);
    CHECK(gstd_accept(db, &r, components, "repo-volume", second) == GSTD_ACCEPT_REPLAY &&
          !strcmp(first, second));
    json_object_set_new(r.params, "allowProvision", json_false());
    CHECK(gstd_accept(db, &r, components, "repo-volume", second) == GSTD_ACCEPT_REPLAY);
    json_object_set_new(r.params, "components", json_pack("[s,s,s]", "controller", "git", "api"));
    CHECK(gstd_accept(db, &r, components, "repo-volume", second) == GSTD_ACCEPT_CONFLICT);
    json_object_del(r.params, "components");
    json_object_set_new(r.params, "allowProvision", json_true());
    CHECK(gstd_accept(db, &r, components, "repo-volume", second) == GSTD_ACCEPT_CONFLICT);
    json_object_del(r.params, "allowProvision");
    json_object_set_new(message, "requestId", json_string("reconnected"));
    json_object_set_new(message, "timeoutMs", json_integer(1));
    CHECK(!gstd_request_parse(message, &r));
    CHECK(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    CHECK(!gstd_state_open(path, &db));
    CHECK(gstd_accept(db, &r, components, "repo-volume", second) == GSTD_ACCEPT_REPLAY &&
          !strcmp(first, second));
    json_object_set_new(message, "idempotencyKey", json_string("another"));
    CHECK(!gstd_request_parse(message, &r));
    CHECK(gstd_accept(db, &r, components, "repo-volume", second) ==
          GSTD_ACCEPT_DEPLOYMENT_CONFLICT);
    json_object_set_new(r.params, "deploymentId", json_string("d2"));
    CHECK(gstd_accept(db, &r, components, "repo-volume", second) ==
          GSTD_ACCEPT_OWNERSHIP_UNCERTAIN);
done:
    json_decref(components);
    json_decref(message);
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
