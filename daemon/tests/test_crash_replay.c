#define _POSIX_C_SOURCE 200809L
#include "failpoints.h"
#include "gitstored/operations.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "support.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int count(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return -1;
    int n = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : -1;
    sqlite3_finalize(s);
    return n;
}
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int scenario(int boundary) {
    int result = 1;
    char *root = gstd_test_dir();
    char path[512], original[65], replay[65];
    sqlite3 *db = NULL;
    json_t *message = json_pack(
        "{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:s}}", "version", "major", 1, "minor", 0, "requestId",
        "before-disconnect", "method", "deployment.start", "idempotencyKey", "stable", "params",
        "deploymentId", "deployment", "profileId", "core", "profileVersion", "v1");
    json_t *components = json_pack("[s,s,s]", "api", "git", "controller");
    gstd_request request;
    REQUIRE(root && message && components);
    REQUIRE(snprintf(path, sizeof(path), "%s/crash.db", root) > 0);
    REQUIRE(!gstd_state_open(path, &db));
    REQUIRE(sqlite3_exec(db,
                         "INSERT INTO profiles VALUES('core','v1','source','digest','linux-arm64')",
                         NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    REQUIRE(!gstd_request_parse(message, &request));
    pid_t child = fork();
    REQUIRE(child >= 0);
    if (!child) {
        if (gstd_state_open(path, &db))
            _exit(91);
        if (boundary == 0)
            kill(getpid(), SIGKILL);
        if (boundary == 1)
            if (gstd_test_crash_before_commit(db) != SQLITE_OK)
                _exit(97);
        if (gstd_accept(db, &request, components, "volume", original) != GSTD_ACCEPT_NEW)
            _exit(92);
        if (boundary >= 3 && gstd_schedule_take(db, 0, replay) != 1)
            _exit(93);
        if (boundary >= 4 &&
            sqlite3_exec(db,
                         "UPDATE resource_ownership SET "
                         "lifecycle_stage='allocated',backend_identity='controlled-receipt' WHERE "
                         "resource_type='repository-volume'",
                         NULL, NULL, NULL) != SQLITE_OK)
            _exit(94);
        const char *terminal =
            boundary == 5
                ? "UPDATE operations SET state='failed',error='{\"code\":\"CONTROLLED_FAILURE\"}'"
            : boundary == 6 ? "UPDATE operations SET state='cancelled',result='{\"stopped\":true}'"
            : boundary == 7 ? "UPDATE operations SET state='succeeded',result='{\"ready\":true}'"
                            : NULL;
        if (terminal && sqlite3_exec(db, terminal, NULL, NULL, NULL) != SQLITE_OK)
            _exit(96);
        /* No ACK has been emitted; an unrelated observer has no cancellation role. */
        kill(getpid(), SIGKILL);
        _exit(95);
    }
    int status;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    REQUIRE(!gstd_state_open(path, &db));
    int committed = boundary >= 2;
    REQUIRE(count(db, "SELECT count(*) FROM operations") == committed);
    REQUIRE(count(db, "SELECT count(*) FROM dedupe") == committed);
    REQUIRE(count(db, "SELECT count(*) FROM jobs") == committed);
    REQUIRE(count(db, "SELECT count(*) FROM resource_ownership") == committed);
    REQUIRE(gstd_accept(db, &request, components, "volume", original) ==
            (committed ? GSTD_ACCEPT_REPLAY : GSTD_ACCEPT_NEW));
    json_object_set_new(message, "requestId", json_string("reconnected"));
    REQUIRE(!gstd_request_parse(message, &request));
    REQUIRE(gstd_accept(db, &request, components, "volume", replay) == GSTD_ACCEPT_REPLAY);
    REQUIRE(!strcmp(original, replay));
    if (boundary == 3 || boundary == 4)
        REQUIRE(count(db, "SELECT count(*) FROM operations WHERE state='running'") == 1);
    if (boundary == 5) {
        REQUIRE(count(db, "SELECT count(*) FROM operations WHERE state='failed' AND "
                          "error='{\"code\":\"CONTROLLED_FAILURE\"}'") == 1);
        REQUIRE(sqlite3_exec(db, "UPDATE operations SET state='succeeded',error=NULL", NULL, NULL,
                             NULL) != SQLITE_OK);
    }
    if (boundary >= 4)
        REQUIRE(count(db,
                      "SELECT count(*) FROM resource_ownership WHERE ownership_state='held' AND "
                      "lifecycle_stage='allocated' AND backend_identity='controlled-receipt'") ==
                1);
    if (boundary == 6)
        REQUIRE(count(db, "SELECT count(*) FROM operations WHERE state='cancelled' AND "
                          "result='{\"stopped\":true}'") == 1);
    if (boundary == 7)
        REQUIRE(count(db, "SELECT count(*) FROM operations WHERE state='succeeded' AND "
                          "result='{\"ready\":true}'") == 1);
    if (boundary >= 5)
        REQUIRE(sqlite3_exec(db, "UPDATE operations SET state='running',result=NULL,error=NULL",
                             NULL, NULL, NULL) != SQLITE_OK);
    json_object_set_new(request.params, "allowProvision", json_true());
    REQUIRE(gstd_accept(db, &request, components, "volume", replay) == GSTD_ACCEPT_CONFLICT);
    REQUIRE(count(db, "SELECT count(*) FROM operations") == 1);
    result = 0;
done:
    json_decref(message);
    json_decref(components);
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
int main(void) {
    for (int boundary = 0; boundary < 8; boundary++)
        if (scenario(boundary))
            return 1;
    return 0;
}
