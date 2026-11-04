#define _POSIX_C_SOURCE 200809L
#include "gitstored/operations.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "monitor.h"
#include "native_exit.h"
#include "support.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
void gstd_test_control_once(sqlite3 *);
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "observer budget line %d: %s\n", __LINE__, #x);                        \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int scalar(sqlite3 *db, const char *id, const char *sql) {
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : -1;
    sqlite3_finalize(s);
    return result;
}
static json_t *start(const char *name) {
    return json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s,s:s,s:s}}", "version", "major", 1, "minor", 0,
                     "requestId", "r", "method", "deployment.start", "idempotencyKey", name,
                     "params", "deploymentId", name, "profileId", "core", "profileVersion", "v1");
}
int main(void) {
    int result = 1;
    char *root = gstd_test_dir(), path[512], ids[9][65], id[65], taken[65];
    sqlite3 *db = NULL;
    json_t *message = NULL, *components = json_pack("[s]", "api");
    CHECK(root && components);
    snprintf(path, sizeof(path), "%s/observe.db", root);
    CHECK(!gstd_state_open(path, &db));
    CHECK(sqlite3_exec(db,
                       "INSERT INTO profiles VALUES('core','v1','source','digest','linux-arm64')",
                       NULL, NULL, NULL) == SQLITE_OK);
    for (unsigned i = 0; i < 9; i++) {
        char name[32];
        snprintf(name, sizeof(name), "deployment-%u", i);
        message = start(name);
        gstd_request request;
        CHECK(message && !gstd_request_parse(message, &request));
        CHECK(gstd_accept(db, &request, components, NULL, ids[i]) == GSTD_ACCEPT_NEW);
        CHECK(gstd_schedule_take(db, 0, taken) == 1 && !strcmp(taken, ids[i]));
        sqlite3_stmt *s = NULL;
        CHECK(sqlite3_prepare_v2(
                  db, "UPDATE operations SET state='succeeded',result='{}' WHERE operation_id=?",
                  -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_text(s, 1, ids[i], -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(s);
        sqlite3_finalize(s);
        CHECK(rc == SQLITE_DONE);
        CHECK(!gstd_schedule_finish(db, ids[i]));
        json_decref(message);
        message = NULL;
    }
    CHECK(sqlite3_exec(db,
                       "UPDATE deployments SET readiness='ready',observed_generation=generation",
                       NULL, NULL, NULL) == SQLITE_OK);
    for (unsigned i = 0; i < 8; i++)
        CHECK(gstd_schedule_observer_reserve(db, ids[i]) == 1);
    CHECK(gstd_schedule_observer_reserve(db, ids[0]) == 0);
#ifdef __linux__
    /* Controlled kernel watches only: no containers or health request. Expired
     * proof must become unready even when every real runtime slot is occupied. */
    gstd_deployment_environment environment = {.database = db};
    gstd_deployment observed = {.environment = &environment,
                                .stage = 1,
                                .count = 2,
                                .credential_source = json_object(),
                                .keeper_death_fd = -1};
    struct timespec now;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &now));
    observed.ready_at_ms = 1;
    observed.monitor.initialized = 1;
    observed.monitor.reported_ready = 1;
    for (unsigned i = 0; i < 2; i++)
        observed.monitor.last_ready[i] = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 - 31000;
    snprintf(observed.target.operation_id, sizeof(observed.target.operation_id), "%s", ids[8]);
    snprintf(observed.target.deployment_id, sizeof(observed.target.deployment_id), "deployment-8");
    observed.target.generation = 1;
    uint64_t birth;
    char boot[37];
    CHECK(!gstd_native_process_start(getpid(), &birth) && !gstd_native_boot_id(boot));
    for (unsigned i = 0; i < 2; i++) {
        observed.workloads[i].death_fd = -1;
        CHECK(!gstd_native_watch_open(getpid(), birth, boot, &observed.workloads[i].death_fd));
    }
    observed.network.pid_fd = -1;
    CHECK(!gstd_native_watch_open(getpid(), birth, boot, &observed.network.pid_fd));
    CHECK(!gstd_native_watch_open(getpid(), birth, boot, &observed.keeper_death_fd));
    CHECK(!gstd_deployment_monitor_poll(&observed) && !observed.monitor.running);
    CHECK(scalar(db, ids[8],
                 "SELECT readiness='failed' FROM deployments WHERE last_operation_id=?") == 1);
    CHECK(scalar(db, ids[8],
                 "SELECT state='succeeded' AND result='{}' FROM operations WHERE operation_id=?") ==
          1);
    for (unsigned i = 0; i < 2; i++)
        close(observed.workloads[i].death_fd);
    close(observed.network.pid_fd);
    close(observed.keeper_death_fd);
    json_decref(observed.credential_source);
#endif

    message = start("queued-start");
    gstd_request request;
    CHECK(message && !gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, components, NULL, id) == GSTD_ACCEPT_NEW);
    CHECK(!gstd_schedule_finish(db, ids[0]));
    CHECK(gstd_schedule_observer_reserve(db, ids[0]) ==
          0); /* Queued public work gets the freed slot. */
    CHECK(gstd_schedule_take(db, 0, taken) == 1 && !strcmp(taken, id));
    json_decref(message);
    message = json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s}}", "version", "major", 1, "minor", 0,
                        "requestId", "stop", "method", "deployment.stop", "idempotencyKey", "stop",
                        "params", "deploymentId", "deployment-0");
    CHECK(message && !gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, NULL, NULL, id) == GSTD_ACCEPT_NEW);
    CHECK(!gstd_schedule_finish(db, ids[1]));
    CHECK(gstd_schedule_observer_reserve(db, ids[0]) ==
          0); /* Explicit stop blocks new health work. */
    json_decref(message);
    message = json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s}}", "version", "major", 1, "minor", 0,
                        "requestId", "cancel", "method", "operation.cancel", "idempotencyKey",
                        "cancel-terminal", "params", "operationId", ids[2]);
    CHECK(message && !gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, NULL, NULL, id) == GSTD_ACCEPT_NEW);
    int slot = scalar(db, ids[2], "SELECT worker_slot FROM jobs WHERE operation_id=?");
    CHECK(slot >= 0);
    gstd_test_control_once(db);
    CHECK(scalar(db, id, "SELECT state='succeeded' FROM operations WHERE operation_id=?") == 1);
    CHECK(scalar(db, ids[2], "SELECT worker_slot FROM jobs WHERE operation_id=?") == slot);
    CHECK(scalar(db, ids[2],
                 "SELECT state='succeeded' AND result='{}' FROM operations WHERE operation_id=?") ==
          1);
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
