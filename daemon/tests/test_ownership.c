#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/operations.h"
#include "gitstored/ownership.h"
#include "gitstored/state.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int observation = -1;
static int observe(const char *resource, const char *identity, int *terminated) {
    (void)identity;
    if (strcmp(resource, "volume") || observation < 0)
        return -1;
    *terminated = observation;
    return 0;
}
int main(void) {
    int result = 0, dir = -1, lock = -1;
    char *root = gstd_test_dir();
    sqlite3 *db = NULL;
    char path[256], id[65];
    json_t *message = NULL, *components = NULL;
    CHECK(root);
    dir = open(root, O_RDONLY | O_DIRECTORY);
    CHECK(dir >= 0);
    lock = gstd_writer_lock(dir, "volume");
    CHECK(lock >= 0);
    CHECK(gstd_writer_lock(dir, "volume") < 0);
    CHECK(gstd_writer_lock(dir, "../escape") < 0);
    CHECK(snprintf(path, sizeof(path), "%s/ownership.db", root) > 0);
    CHECK(!gstd_state_open(path, &db));
    CHECK(sqlite3_exec(db,
                       "INSERT INTO profiles VALUES('core','v1','source','digest','linux-arm64')",
                       NULL, NULL, NULL) == SQLITE_OK);
    const char *request =
        "{\"version\":{\"major\":1,\"minor\":0},\"requestId\":\"r\",\"method\":\"deployment."
        "start\",\"idempotencyKey\":\"initial\",\"params\":{\"deploymentId\":\"d1\",\"profileId\":"
        "\"core\",\"profileVersion\":\"v1\"}}";
    message = gstd_json_parse((const unsigned char *)request, strlen(request));
    CHECK(message);
    components = json_pack("[s]", "git");
    CHECK(components);
    gstd_request r;
    CHECK(!gstd_request_parse(message, &r));
    CHECK(gstd_accept(db, &r, components, "volume", id) == GSTD_ACCEPT_NEW);
    CHECK(gstd_ownership_reconcile(db, "volume", "unrelated", observe) < 0);
    CHECK(gstd_ownership_reconcile(db, "volume", "d1", observe) < 0);
    CHECK(gstd_writer_lock(dir, "volume") < 0);
    observation = 0;
    CHECK(gstd_ownership_reconcile(db, "volume", "d1", observe) == 0);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    CHECK(!gstd_state_open(path, &db));
    observation = -1;
    CHECK(gstd_ownership_reconcile(db, "volume", "d1", observe) < 0);
    observation = 1;
    CHECK(gstd_ownership_reconcile(db, "volume", "d1", observe) == 1);
    close(lock);
    lock = -1;
    lock = gstd_writer_lock(dir, "volume");
    CHECK(lock >= 0);
done:
    if (lock >= 0)
        close(lock);
    if (dir >= 0)
        close(dir);
    json_decref(components);
    json_decref(message);
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
