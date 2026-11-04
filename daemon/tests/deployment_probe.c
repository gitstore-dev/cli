#define _GNU_SOURCE 1
#include "cleanup.h"
#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "recovery.h"
#include "restoration.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "asset probe line %d\n", __LINE__);                                    \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int wait_fs(gstd_filesystem_job *job, gstd_filesystem_reply *reply) {
    for (;;) {
        int ready = gstd_filesystem_poll(job, -1, reply);
        if (ready)
            return ready;
        struct timespec t = {0, 1000000};
        nanosleep(&t, NULL);
    }
}
static int descriptor_count(void) {
    DIR *directory = opendir("/proc/self/fd");
    if (!directory)
        return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (entry->d_name[0] != '.')
            count++;
    closedir(directory);
    return count;
}
/* Explicit developer fixture paths only; not linked into an installed CLI. */
int main(int argc, char **argv) {
    CHECK((argc == 6 || (argc == 7 && !strcmp(argv[6], "restore-observers"))) && geteuid());
    alarm(600);
    CHECK(!prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0));
    struct rlimit core = {0, 0};
    CHECK(!setrlimit(RLIMIT_CORE, &core));
    gstd_json_initialize();
    int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
        source = open(argv[3], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(root >= 3 && source >= 3);
    char path[4096];
    CHECK(snprintf(path, sizeof(path), "%s/state.db", argv[1]) > 0);
    sqlite3 *db = NULL;
    CHECK(!gstd_state_open(path, &db));
    json_t *components = json_pack("[s,s,s]", "api", "git", "controller");
#if defined(__aarch64__)
    const char *arch = "arm64", *platform = "linux-arm64";
#else
    const char *arch = "amd64", *platform = "linux-amd64";
#endif
    json_t *manifest = gstd_asset_manifest(components, arch);
    CHECK(manifest);
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(db, "INSERT INTO profiles VALUES('core','1',?,?,?)", -1, &s, NULL) ==
          SQLITE_OK);
    sqlite3_bind_text(s, 1, json_string_value(json_object_get(manifest, "serverRevision")), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, json_string_value(json_object_get(manifest, "profileDigest")), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, platform, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    const char start[] =
        "{\"version\":{\"major\":1,\"minor\":0},\"requestId\":\"r\",\"method\":\"deployment."
        "start\",\"idempotencyKey\":\"asset-actor\",\"params\":{\"deploymentId\":\"one\","
        "\"profileId\":\"core\",\"profileVersion\":\"1\"}}";
    json_t *message = gstd_json_parse((const unsigned char *)start, sizeof(start) - 1);
    gstd_request request;
    char op[65];
    CHECK(message && !gstd_request_parse(message, &request) &&
          gstd_accept(db, &request, components, "repositories", op) == GSTD_ACCEPT_NEW);
    gstd_deployment_target target = {0};
    CHECK(!gstd_deployment_target_read(db, op, &target));
    gstd_cache_owner expected;
    CHECK(gstd_asset_record_cache_state(db, &expected) == 1);
    gstd_filesystem_input input = {.action = GSTD_FS_INITIALIZE,
                                   .store_id = target.store_id,
                                   .expected_cache = expected,
                                   .descriptors = {root, -1, -1}};
    gstd_filesystem_job job = {0};
    gstd_filesystem_reply initialized = {0};
    CHECK(!gstd_filesystem_launch(&input, 5000, &job) && wait_fs(&job, &initialized) == 1 &&
          !initialized.status);
    gstd_cache_owner cache = initialized.cache_owner;
    cache.lock_fd = initialized.descriptors[1];
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!gstd_asset_record_cache_bind(db, &cache));
    CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    gstd_runtime_settings settings = GSTD_RUNTIME_SETTINGS_INITIAL;
    CHECK(!gstd_runtime_settings_load(root, &settings));
    gstd_deployment_environment environment = {.database = db,
                                               .settings = &settings,
                                               .cache_fd = initialized.descriptors[0],
                                               .resource_fd = initialized.descriptors[4],
                                               .native_fd = initialized.descriptors[5],
                                               .broker_fd =
                                                   open(argv[2], O_RDONLY | O_NOFOLLOW | O_CLOEXEC),
                                               .volume_root = initialized.volume_root};
    environment.volume_root.root_fd = initialized.descriptors[2];
    environment.volume_root.lock_fd = initialized.descriptors[3];
    CHECK(environment.broker_fd >= 3);
    char scheduled[65];
    CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(op, scheduled));
    int descriptors_before = descriptor_count();
    gstd_deployment deployment = {0}, restored = {0};
    gstd_deployment *active = &deployment;
    int restoration_outcome = 0;
    CHECK(!gstd_deployment_begin(&environment, op, &deployment));
    int done = 0, last_stage = -1;
    while (!done) {
        done = gstd_deployment_poll(&deployment, -1);
        CHECK(done >= 0);
        if (deployment.stage != last_stage) {
            fprintf(stderr, "deployment stage %d component %u\n", deployment.stage,
                    deployment.index);
            last_stage = deployment.stage;
        }
        if (!done) {
            struct timespec t = {0, 10000000};
            nanosleep(&t, NULL);
        }
    }
    int start_outcome = deployment.outcome;
    fprintf(stderr, "start outcome=%d detail=%d\n", start_outcome, deployment.detail);
    CHECK(!gstd_schedule_finish(db, op));
    if (argc == 7 && !start_outcome) {
        CHECK(!gstd_recovery_invalidate(db));
        gstd_restoration restoration = {0};
        restoration_outcome = gstd_restoration_begin(&environment, op, &restored, &restoration);
        if (!restoration_outcome) {
            int finished = 0;
            while (!finished) {
                finished = gstd_restoration_poll(&restoration);
                if (finished < 0) {
                    restoration_outcome = -2;
                    break;
                }
                if (!finished) {
                    struct timespec delay = {0, 1000000};
                    nanosleep(&delay, NULL);
                }
            }
            if (finished == 1)
                restoration_outcome = restoration.outcome;
        }
        if (!restoration_outcome) {
            CHECK(restored.recovery_proven && !gstd_deployment_observers_close(&deployment));
            active = &restored;
        } else {
            /* The original observer is still held. Restore only its own ledger
             * state for safe fixture cleanup; no production recovery fallback. */
            (void)gstd_restoration_discard(&restoration);
            CHECK(sqlite3_prepare_v2(db,
                                     "UPDATE resource_ownership SET ownership_state='held' WHERE "
                                     "operation_id=? AND ownership_state='uncertain'",
                                     -1, &s, NULL) == SQLITE_OK);
            sqlite3_bind_text(s, 1, op, -1, SQLITE_TRANSIENT);
            CHECK(sqlite3_step(s) == SQLITE_DONE);
            sqlite3_finalize(s);
        }
        fprintf(stderr, "observer restoration outcome=%d\n", restoration_outcome);
    }
    json_t *stop = json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s}}", "version", "major", 1, "minor", 0,
                             "requestId", "stop", "method", "deployment.stop", "idempotencyKey",
                             "stop-actor", "params", "deploymentId", "one");
    gstd_request stop_request;
    char stop_operation[65];
    CHECK(stop && !gstd_request_parse(stop, &stop_request));
    CHECK(gstd_accept(db, &stop_request, components, NULL, stop_operation) == GSTD_ACCEPT_NEW);
    CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(stop_operation, scheduled));
    gstd_cleanup cleanup = {0};
    CHECK(!gstd_cleanup_begin(active, stop_operation, &cleanup));
    done = 0;
    while (!done) {
        done = gstd_cleanup_poll(&cleanup);
        CHECK(done >= 0);
        if (!done) {
            struct timespec t = {0, 10000000};
            nanosleep(&t, NULL);
        }
    }
    fprintf(stderr, "cleanup outcome=%d\n", cleanup.outcome);
    CHECK(!gstd_schedule_finish(db, stop_operation));
    CHECK(!start_outcome && !cleanup.outcome);
    CHECK(!gstd_cleanup_dispose(&cleanup));
    CHECK(descriptor_count() == descriptors_before);
    CHECK(!restoration_outcome);
    puts("{\"deploymentActor\":\"ready\",\"stop\":\"observed\",\"volumes\":\"retained\",\"scope\":"
         "\"private actor regression; public evidence separate\"}");
    return 0;
}
