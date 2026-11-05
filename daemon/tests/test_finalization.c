#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "cleanup.h"
#include "gitstored/operations.h"
#include "gitstored/ownership.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "monitor.h"
#include "support.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
int gstd_test_deployment_finish_failure(gstd_deployment *);
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "finalization line %d: %s\n", __LINE__, #x);                           \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
#define SQL(x) CHECK(sqlite3_exec(db, x, NULL, NULL, NULL) == SQLITE_OK)
static int state(sqlite3 *db, const char *id, const char *expected) {
    sqlite3_stmt *s = NULL;
    int ok = sqlite3_prepare_v2(db, "SELECT state FROM operations WHERE operation_id=?", -1, &s,
                                NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(s) == SQLITE_ROW &&
             !strcmp((const char *)sqlite3_column_text(s, 0), expected);
    }
    sqlite3_finalize(s);
    return ok;
}
static int owned(sqlite3 *db, const char *id, const char *expected) {
    sqlite3_stmt *s = NULL;
    int ok = sqlite3_prepare_v2(
                 db, "SELECT ownership_state FROM resource_ownership WHERE operation_id=?", -1, &s,
                 NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(s) == SQLITE_ROW &&
             !strcmp((const char *)sqlite3_column_text(s, 0), expected);
    }
    sqlite3_finalize(s);
    return ok;
}
static int scenario(int expired) {
    int result = 1, dir = -1, acquired = -1;
    char *root = gstd_test_dir(), path[512], source[65], stop[65], scheduled[65], replay[65];
    sqlite3 *db = NULL;
    json_t *message = NULL, *components = NULL;
    gstd_deployment_environment environment = {0};
    gstd_deployment deployment = {.volume = {.data_fd = -1, .writer_fd = -1}};
    gstd_cleanup cleanup = {0};
    CHECK(root);
    snprintf(path, sizeof(path), "%s/state.db", root);
    CHECK(!gstd_state_open(path, &db));
    SQL("INSERT INTO profiles VALUES('core','1','source','digest','linux-arm64')");
    message =
        json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:s}}", "version", "major", 1, "minor", 0,
                  "requestId", "start", "method", "deployment.start", "idempotencyKey", "start",
                  "params", "deploymentId", "one", "profileId", "core", "profileVersion", "1");
    components = json_pack("[s]", "git");
    gstd_request request;
    CHECK(message && components && !gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, components, "volume", source) == GSTD_ACCEPT_NEW);
    CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(scheduled, source));
    environment.database = db;
    deployment.environment = &environment;
    deployment.stage = 1;
    deployment.target.generation = 1;
    memcpy(deployment.target.operation_id, source, strlen(source) + 1);
    memcpy(deployment.target.deployment_id, "one", 4);
    /* A known pre-allocation failure has no native effects. Test the actual
     * terminal transaction and retry loop, without constructing fake workloads. */
    SQL("PRAGMA query_only=ON");
    CHECK(!gstd_test_deployment_finish_failure(&deployment));
    CHECK(deployment.finalizing && deployment.outcome == -2 && state(db, source, "running"));
    CHECK(!gstd_deployment_poll(&deployment, -1) && owned(db, source, "held"));
    CHECK(gstd_deployment_observers_close(&deployment) == -1);
    CHECK(gstd_deployment_monitor_observation_blocked(&deployment, "one", source));
    CHECK(!gstd_deployment_monitor_observation_blocked(&deployment, "foreign", source));
    CHECK(sqlite3_get_autocommit(db));
    SQL("PRAGMA query_only=OFF");
    CHECK(gstd_deployment_poll(&deployment, -1) == 1 && !deployment.finalizing);
    CHECK(state(db, source, "failed") && deployment.outcome == -1);
    CHECK(!gstd_deployment_monitor_observation_blocked(&deployment, "one", source));
    CHECK(!gstd_schedule_finish(db, source));
    CHECK(gstd_accept(db, &request, components, "volume", replay) == GSTD_ACCEPT_REPLAY &&
          !strcmp(source, replay));
    json_decref(message);
    message = json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s}}", "version", "major", 1, "minor", 0,
                        "requestId", "stop", "method", "deployment.stop", "idempotencyKey", "stop",
                        "params", "deploymentId", "one");
    CHECK(message && !gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, NULL, NULL, stop) == GSTD_ACCEPT_NEW);
    CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(scheduled, stop));
    deployment.persistent = 1;
    deployment.datastore.helper.stage = 1;
    CHECK(gstd_cleanup_begin(&deployment, stop, &cleanup) == -1 && sqlite3_get_autocommit(db) &&
          !cleanup.stage);
    deployment.datastore.helper.stage = 0;
    deployment.persistent = 0;
    CHECK(!gstd_cleanup_begin(&deployment, stop, &cleanup));
    dir = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(dir >= 3);
    /* Real advisory lease proves the parent retains exclusivity while recording
     * is unavailable. No volume or container was allocated by this actor. */
    deployment.volume.writer_fd = gstd_writer_lock(dir, "volume");
    CHECK(deployment.volume.writer_fd >= 3);
    if (expired)
        cleanup.deadline_ms = 0;
#ifdef __linux__
    if (expired == 2) {
        gstd_filesystem_input input = {.action = GSTD_FS_INITIALIZE,
                                       .store_id = "0123456789abcdef0123456789abcdef",
                                       .descriptors = {dir, -1, -1},
                                       .test_pause = 1};
        CHECK(!gstd_filesystem_launch(&input, 5000, &cleanup.filesystem));
        cleanup.stage = GSTD_CLEANUP_WRITER;
    }
#endif
    SQL("PRAGMA query_only=ON");
    for (unsigned i = 0; i < 1000 && !cleanup.finalizing; i++) {
        CHECK(!gstd_cleanup_poll(&cleanup));
        struct timespec delay = {0, 1000000};
        if (!cleanup.finalizing)
            nanosleep(&delay, NULL);
    }
    CHECK(cleanup.finalizing && cleanup.outcome == -2 && !cleanup.filesystem.worker);
    CHECK(!gstd_cleanup_poll(&cleanup));
    CHECK(state(db, stop, "running") && state(db, source, "failed") && owned(db, source, "held"));
    CHECK(gstd_writer_lock(dir, "volume") < 0);
    CHECK(gstd_cleanup_dispose(&cleanup) == -1 && gstd_cleanup_retry_begin(&cleanup, stop) == -1);
    CHECK(sqlite3_get_autocommit(db));
    SQL("PRAGMA query_only=OFF");
    CHECK(gstd_cleanup_poll(&cleanup) == 1 && !cleanup.finalizing);
    CHECK(state(db, source, "failed"));
    CHECK(state(db, stop, expired ? "failed" : "succeeded"));
    CHECK(owned(db, source, expired ? "held" : "released"));
    if (expired) {
        CHECK(cleanup.outcome == -2 && deployment.volume.writer_fd >= 3);
        CHECK(gstd_writer_lock(dir, "volume") < 0);
    } else {
        CHECK(!cleanup.outcome && deployment.volume.writer_fd == -1);
        acquired = gstd_writer_lock(dir, "volume");
        CHECK(acquired >= 3);
    }
    CHECK(!gstd_schedule_finish(db, stop));
    CHECK(gstd_accept(db, &request, NULL, NULL, replay) == GSTD_ACCEPT_REPLAY &&
          !strcmp(replay, stop));
    result = 0;
done:
#ifdef __linux__
    if (cleanup.filesystem.worker) {
        kill(cleanup.filesystem.worker, SIGKILL);
        waitpid(cleanup.filesystem.worker, NULL, 0);
        close(cleanup.filesystem.channel);
    }
#endif
    if (deployment.volume.writer_fd >= 3)
        close(deployment.volume.writer_fd);
    if (acquired >= 3)
        close(acquired);
    if (dir >= 3)
        close(dir);
    json_decref(message);
    json_decref(components);
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
int main(void) {
    gstd_json_initialize();
    return scenario(0) || scenario(1)
#ifdef __linux__
           || scenario(2)
#endif
        ;
}
