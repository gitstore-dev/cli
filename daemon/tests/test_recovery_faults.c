#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/operations.h"
#include "gitstored/ownership.h"
#include "gitstored/state.h"
#include "support.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int uncertain(const char *resource, const char *identity, int *terminated) {
    (void)resource;
    (void)identity;
    *terminated = -1;
    return -1;
}
int main(void) {
    alarm(20);
    int result = 1, dir = -1, marker = -1, lock = -1;
    int ready[2] = {-1, -1}, release[2] = {-1, -1};
    pid_t child = -1;
    char *root = gstd_test_dir();
    char path[512], bad[512], original[65], replay[65], sql[128];
    sqlite3 *db = NULL, *invalid = NULL;
    json_t *message = json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:s}}", "version", "major", 1,
                                "minor", 0, "requestId", "faults", "method", "deployment.start",
                                "idempotencyKey", "stable", "params", "deploymentId", "deployment",
                                "profileId", "core", "profileVersion", "v1");
    json_t *components = json_pack("[s]", "git");
    gstd_request request;
    REQUIRE(root && message && components);
    dir = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    REQUIRE(dir >= 0);
    marker = openat(dir, "retained", O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    REQUIRE(marker >= 0 && write(marker, "retained-before-fault", 21) == 21 && !fsync(marker));
    REQUIRE(snprintf(path, sizeof(path), "%s/state.db", root) > 0);
    REQUIRE(!gstd_state_open(path, &db));
    REQUIRE(sqlite3_exec(
                db,
                "INSERT INTO profiles VALUES('core','v1','source','digest','linux-arm64');CREATE "
                "TABLE pressure(bytes BLOB)",
                NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(!gstd_request_parse(message, &request));
    REQUIRE(gstd_accept(db, &request, components, "volume", original) == GSTD_ACCEPT_NEW);
    /* An actual separate process owns the canonical writer while observation is uncertain. */
    REQUIRE(!pipe(ready) && !pipe(release));
    child = fork();
    REQUIRE(child >= 0);
    if (!child) {
        close(ready[0]);
        close(release[1]);
        int lease = gstd_writer_lock(dir, "volume");
        char byte;
        if (lease < 0 || write(ready[1], "r", 1) != 1 || read(release[0], &byte, 1) != 1)
            _exit(90);
        close(lease);
        _exit(0);
    }
    close(ready[1]);
    ready[1] = -1;
    close(release[0]);
    release[0] = -1;
    char byte;
    REQUIRE(read(ready[0], &byte, 1) == 1);
    REQUIRE(gstd_writer_lock(dir, "volume") < 0);
    REQUIRE(gstd_ownership_reconcile(db, "volume", "deployment", uncertain) == -1);
    REQUIRE(gstd_writer_lock(dir, "volume") < 0);
    /* Limit this disposable database, never fill the host filesystem. */
    sqlite3_stmt *statement = NULL;
    REQUIRE(sqlite3_prepare_v2(db, "PRAGMA page_count", -1, &statement, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
    int pages = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    REQUIRE(snprintf(sql, sizeof(sql), "PRAGMA max_page_count=%d", pages) > 0);
    REQUIRE(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "INSERT INTO pressure VALUES(zeroblob(1000000))", NULL, NULL, NULL) ==
            SQLITE_FULL);
    REQUIRE(sqlite3_get_autocommit(db));
    REQUIRE(gstd_accept(db, &request, components, "volume", replay) == GSTD_ACCEPT_REPLAY);
    REQUIRE(!strcmp(original, replay));
    REQUIRE(gstd_writer_lock(dir, "volume") < 0);
    REQUIRE(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    REQUIRE(!gstd_state_open(path, &db));
    REQUIRE(gstd_accept(db, &request, components, "volume", replay) == GSTD_ACCEPT_REPLAY &&
            !strcmp(original, replay));
    /* A full metadata record budget must preserve recent accepted history and
     * return an explicit pre-effect limit. This seeds records, not workloads. */
    REQUIRE(sqlite3_exec(
                db,
                "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<99999) "
                "INSERT INTO operations SELECT 'pressure-'||x,0,'operation.cancel',NULL,NULL,"
                "'0000000000000000000000000000000000000000000000000000000000000000',"
                "'{}',strftime('%Y-%m-%dT%H:%M:%SZ','now'),strftime('%Y-%m-%dT%H:%M:%SZ','now'),"
                "'cancelled','normal','{}',NULL FROM n",
                NULL, NULL, NULL) == SQLITE_OK);
    json_object_set_new(message, "idempotencyKey", json_string("new-at-full-store"));
    json_object_set_new(json_object_get(message, "params"), "deploymentId",
                        json_string("new-deployment"));
    REQUIRE(!gstd_request_parse(message, &request));
    REQUIRE(gstd_accept(db, &request, components, "new-volume", replay) == GSTD_ACCEPT_LIMIT);
    json_object_set_new(message, "idempotencyKey", json_string("stable"));
    json_object_set_new(json_object_get(message, "params"), "deploymentId",
                        json_string("deployment"));
    REQUIRE(!gstd_request_parse(message, &request));
    REQUIRE(gstd_accept(db, &request, components, "volume", replay) == GSTD_ACCEPT_REPLAY &&
            !strcmp(original, replay));
    char bytes[21];
    REQUIRE(pread(marker, bytes, sizeof(bytes), 0) == sizeof(bytes) &&
            !memcmp(bytes, "retained-before-fault", 21));
    REQUIRE(write(release[1], "x", 1) == 1);
    close(release[1]);
    release[1] = -1;
    int status;
    REQUIRE(waitpid(child, &status, 0) == child);
    child = -1;
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    lock = gstd_writer_lock(dir, "volume");
    REQUIRE(lock >= 0);
    /* Corrupt input is refused without overwriting it or touching retained bytes. */
    REQUIRE(snprintf(bad, sizeof(bad), "%s/corrupt.db", root) > 0);
    int fd = open(bad, O_CREAT | O_EXCL | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    REQUIRE(fd >= 0);
    int written = write(fd, "invalid database fixture", 24) == 24 && !fsync(fd);
    close(fd);
    REQUIRE(written);
    REQUIRE(gstd_state_open(bad, &invalid) != 0 && !invalid);
    fd = open(bad, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    REQUIRE(fd >= 0);
    char corrupt[25] = {0};
    int unchanged = read(fd, corrupt, sizeof(corrupt)) == 24 &&
                    !memcmp(corrupt, "invalid database fixture", 24);
    close(fd);
    REQUIRE(unchanged);
    REQUIRE(pread(marker, bytes, sizeof(bytes), 0) == sizeof(bytes) &&
            !memcmp(bytes, "retained-before-fault", 21));
    result = 0;
done:
    for (int i = 0; i < 2; i++) {
        if (ready[i] >= 0)
            close(ready[i]);
        if (release[i] >= 0)
            close(release[i]);
    }
    if (child > 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    if (lock >= 0)
        close(lock);
    if (marker >= 0)
        close(marker);
    if (dir >= 0)
        close(dir);
    if (db)
        sqlite3_close(db);
    if (invalid)
        sqlite3_close(invalid);
    json_decref(message);
    json_decref(components);
    if (root)
        gstd_test_remove(root);
    return result;
}
