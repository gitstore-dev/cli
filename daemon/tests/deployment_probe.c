#define _GNU_SOURCE 1
#include "cleanup.h"
#include "datastore.h"
#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "image_backend.h"
#include "recovery.h"
#include "restoration.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
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
int gstd_test_datastore_live(gstd_datastore *, const gstd_filesystem_reply *);
int main(int argc, char **argv) {
    int persistent_live = argc == 7 && !strcmp(argv[6], "start-scylla");
    int persistent_leave = argc == 7 && !strcmp(argv[6], "leave-actor-scylla");
    int persistent_copy = argc == 7 && !strcmp(argv[6], "reject-copy-scylla");
    int persistent_population = argc == 7 && !strcmp(argv[6], "reject-population-scylla");
    int persistent_recover = persistent_population || persistent_copy ||
                             (argc == 7 && !strcmp(argv[6], "recover-actor-scylla"));
    int persistent_restore = argc == 7 && !strcmp(argv[6], "restore-actor-scylla");
    int persistent_repeat =
        persistent_restore || (argc == 7 && !strcmp(argv[6], "restart-actor-scylla"));
    int persistent_actor = persistent_leave || persistent_recover || persistent_repeat ||
                           (argc == 7 && !strcmp(argv[6], "actor-scylla"));
    int persistent_preparation =
        persistent_live || (argc == 7 && !strcmp(argv[6], "prepare-scylla"));
    CHECK((argc == 6 || (argc == 7 && (!strcmp(argv[6], "restore-observers") ||
                                       persistent_preparation || persistent_actor))) &&
          geteuid());
    if (((persistent_preparation || persistent_actor) && !gstd_image_backend_available()) ||
        ((persistent_live || persistent_actor) && !gstd_native_backend_available())) {
        fputs("Selected datastore probe requires image SDK and native runtime linkage\n", stderr);
        return 77;
    }
    alarm(600);
    CHECK(!prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0));
    struct rlimit core = {0, 0};
    CHECK(!setrlimit(RLIMIT_CORE, &core));
    gstd_json_initialize();
    int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
        source = open(argv[3], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(root >= 3 && source >= 3);
    int installed = open(argv[2], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat installed_info;
    CHECK(installed >= 3);
    if (!persistent_preparation) {
        CHECK(!fstat(installed, &installed_info) && S_ISREG(installed_info.st_mode) &&
              installed_info.st_nlink == 1 && !(installed_info.st_mode & 0022) &&
              (installed_info.st_mode & 0111) &&
              (installed_info.st_uid == 0 || installed_info.st_uid == geteuid()));
    }
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
    json_t *manifest = gstd_asset_manifest_for_datastore(
        components, arch, persistent_preparation || persistent_actor ? "scylladb" : "memdb");
    CHECK(manifest);
    sqlite3_stmt *s = NULL;
    if (!persistent_recover) {
        CHECK(sqlite3_prepare_v2(db, "INSERT INTO profiles VALUES('core','1',?,?,?)", -1, &s,
                                 NULL) == SQLITE_OK);
        sqlite3_bind_text(s, 1, json_string_value(json_object_get(manifest, "serverRevision")), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, json_string_value(json_object_get(manifest, "profileDigest")), -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 3, platform, -1, SQLITE_TRANSIENT);
        CHECK(sqlite3_step(s) == SQLITE_DONE);
        sqlite3_finalize(s);
    }
    const char start[] =
        "{\"version\":{\"major\":1,\"minor\":0},\"requestId\":\"r\",\"method\":\"deployment."
        "start\",\"idempotencyKey\":\"asset-actor\",\"params\":{\"deploymentId\":\"one\","
        "\"profileId\":\"core\",\"profileVersion\":\"1\"}}";
    json_t *message = gstd_json_parse((const unsigned char *)start, sizeof(start) - 1);
    if ((persistent_preparation || persistent_actor) && message)
        CHECK(!json_object_set_new(json_object_get(message, "params"), "datastore",
                                   json_string("scylladb")));
    gstd_request request;
    char op[65];
    gstd_deployment_target target = {0};
    if (persistent_recover) {
        CHECK(sqlite3_prepare_v2(db,
                                 "SELECT operation_id FROM operations WHERE "
                                 "method='deployment.start' ORDER BY rowid DESC LIMIT 1",
                                 -1, &s, NULL) == SQLITE_OK);
        CHECK(sqlite3_step(s) == SQLITE_ROW);
        const char *prior = (const char *)sqlite3_column_text(s, 0);
        CHECK(prior && strlen(prior) < sizeof(op));
        memcpy(op, prior, strlen(prior) + 1);
        sqlite3_finalize(s);
        CHECK(!gstd_deployment_target_recovery_read(db, op, &target));
    } else {
        CHECK(message && !gstd_request_parse(message, &request) &&
              gstd_accept(db, &request, components, "repositories", op) == GSTD_ACCEPT_NEW);
        CHECK(!gstd_deployment_target_read(db, op, &target));
    }
    gstd_cache_owner expected;
    CHECK(gstd_asset_record_cache_state(db, &expected) == (persistent_recover ? 0 : 1));
    gstd_filesystem_input input = {.action = GSTD_FS_INITIALIZE,
                                   .store_id = target.store_id,
                                   .expected_cache = expected,
                                   .descriptors = {root, -1, -1}};
    gstd_filesystem_job job = {0};
    gstd_filesystem_reply initialized = {0};
    CHECK(!gstd_filesystem_launch(&input, 5000, &job) && wait_fs(&job, &initialized) == 1);
    if (persistent_copy) {
        CHECK(initialized.status && !initialized.descriptor_count);
        gstd_filesystem_reply_close(&initialized);
        gstd_deployment_target_clear(&target);
        json_decref(message);
        json_decref(manifest);
        json_decref(components);
        close(installed);
        close(source);
        close(root);
        CHECK(sqlite3_close(db) == SQLITE_OK);
        puts("{\"copiedStateRejected\":true,\"nativeDispatched\":false}");
        return 0;
    }
    CHECK(!initialized.status);
    gstd_cache_owner cache = initialized.cache_owner;
    cache.lock_fd = initialized.descriptors[1];
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!gstd_asset_record_cache_bind(db, &cache));
    CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    gstd_runtime_settings settings = GSTD_RUNTIME_SETTINGS_INITIAL;
    CHECK(!gstd_runtime_settings_load(root, &settings));
    gstd_deployment_environment environment = {.database = db,
                                               .state_fd = root,
                                               .settings = &settings,
                                               .cache_fd = initialized.descriptors[0],
                                               .resource_fd = initialized.descriptors[4],
                                               .native_fd = initialized.descriptors[5],
                                               .broker_fd = installed,
                                               .volume_root = initialized.volume_root};
    environment.volume_root.root_fd = initialized.descriptors[2];
    environment.volume_root.lock_fd = initialized.descriptors[3];
    CHECK(environment.broker_fd >= 3);
    char scheduled[65];
    if (!persistent_recover)
        CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(op, scheduled));
    if (persistent_preparation) {
        char configuration[65];
        CHECK(snprintf(configuration, sizeof(configuration), "cfg-%s", op) > 0);
        CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
        CHECK(!gstd_deployment_resource_reserve(db, &target, configuration, "configuration"));
        CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
        input = (gstd_filesystem_input){.action = GSTD_FS_SCOPE,
                                        .store_id = target.store_id,
                                        .operation_id = op,
                                        .deployment_id = target.deployment_id,
                                        .generation = target.generation,
                                        .resource_id = configuration,
                                        .resource_kind = "configuration",
                                        .descriptors = {environment.resource_fd, -1, -1}};
        gstd_filesystem_reply config = {0};
        CHECK(!gstd_filesystem_launch(&input, 10000, &job) && wait_fs(&job, &config) == 1 &&
              !config.status);
        CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
        CHECK(!gstd_deployment_scope_record(db, &target, configuration, &config));
        CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
        gstd_datastore datastore = {0};
        CHECK(!gstd_datastore_prepare_begin(&environment, &target, configuration, &datastore));
        int ready = 0, previous = -1;
        while (!ready) {
            ready = gstd_datastore_prepare_poll(&datastore, -1);
            CHECK(ready >= 0);
            if (datastore.stage != previous) {
                fprintf(stderr, "datastore preparation stage=%d image=%u\n", datastore.stage,
                        datastore.index);
                previous = datastore.stage;
            }
            if (!ready) {
                struct timespec interval = {0, 10000000};
                nanosleep(&interval, NULL);
            }
        }
        fprintf(stderr, "datastore preparation outcome=%d detail=%d\n", datastore.outcome,
                datastore.detail);
        CHECK(!datastore.outcome && datastore.index == 3 && datastore.graph.fresh_datastore &&
              datastore.graph.stage == GSTD_BOOT_PREPARE);
        for (unsigned i = 0; i < 3; i++)
            CHECK(datastore.workloads[i].rootfs_fd >= 3 && !datastore.workloads[i].launched);
        gstd_volume_handle denied = {.data_fd = -1, .writer_fd = -1};
        CHECK(gstd_scylla_volume_acquire(environment.volume_root.root_fd, &environment.volume_root,
                                         datastore.volume_id, datastore.producer_id, 0,
                                         &datastore.volume.identity, &denied) == GSTD_VOLUME_HELD);
        if (persistent_live)
            CHECK(!gstd_test_datastore_live(&datastore, &config));
        CHECK(!gstd_datastore_observers_close(&datastore));
        gstd_filesystem_reply_close(&config);
        gstd_deployment_target_clear(&target);
        gstd_runtime_settings_clear(&settings);
        close(environment.broker_fd);
        gstd_filesystem_reply_close(&initialized);
        close(source);
        close(root);
        json_decref(message);
        json_decref(manifest);
        json_decref(components);
        CHECK(sqlite3_close(db) == SQLITE_OK);
        if (persistent_live)
            puts("{\"persistentStartProbe\":true,\"publicCapability\":false}");
        else
            puts("{\"persistentPreparation\":true,\"materializations\":3,\"nativeStarted\":false,"
                 "\"writerLeaseHeld\":true,\"publicCapability\":false}");
        return 0;
    }
    for (unsigned cycle = 0; cycle < (persistent_repeat ? 2u : 1u); cycle++) {
        if (cycle) {
            CHECK(!json_object_set_new(message, "idempotencyKey", json_string("retained-actor")));
            CHECK(!gstd_request_parse(message, &request) &&
                  gstd_accept(db, &request, components, "repositories", op) == GSTD_ACCEPT_NEW);
            CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(op, scheduled));
        }
        int descriptors_before = descriptor_count();
        gstd_deployment deployment = {0}, restored = {0};
        gstd_deployment *active = &deployment;
        int restoration_outcome = 0;
        if (!persistent_recover)
            CHECK(!gstd_deployment_begin(&environment, op, &deployment));
        int done = persistent_recover, last_stage = -1;
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
        if (persistent_repeat && cycle && !start_outcome)
            CHECK(!deployment.datastore.graph.fresh_datastore);
        fprintf(stderr, "start outcome=%d detail=%d\n", start_outcome, deployment.detail);
        if (!persistent_recover)
            CHECK(!gstd_schedule_finish(db, op));
        if (persistent_leave && !start_outcome) {
            puts("{\"managerReady\":true,\"publicCapability\":false}");
            CHECK(!fflush(stdout));
            for (;;)
                pause(); /* The selected disposable test kills this owner process. */
        }
        if (argc == 7 &&
            (persistent_recover || persistent_restore || !strcmp(argv[6], "restore-observers")) &&
            !start_outcome) {
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
            if (persistent_population) {
                CHECK(restoration_outcome != 0);
                CHECK(!gstd_restoration_discard(&restoration));
                puts("{\"populationMismatchRejected\":true,\"nativeDispatched\":false}");
                break;
            }
            if (!restoration_outcome) {
                CHECK(restored.recovery_proven);
                if (!persistent_recover)
                    CHECK(!gstd_deployment_observers_close(&deployment));
                active = &restored;
            } else {
                CHECK(!persistent_recover);
                /* The original observer is still held. Restore only its own ledger
                 * state for safe fixture cleanup; no production recovery fallback. */
                (void)gstd_restoration_discard(&restoration);
                CHECK(
                    sqlite3_prepare_v2(db,
                                       "UPDATE resource_ownership SET ownership_state='held' WHERE "
                                       "operation_id=? AND ownership_state='uncertain'",
                                       -1, &s, NULL) == SQLITE_OK);
                sqlite3_bind_text(s, 1, op, -1, SQLITE_TRANSIENT);
                CHECK(sqlite3_step(s) == SQLITE_DONE);
                sqlite3_finalize(s);
            }
            fprintf(stderr, "observer restoration outcome=%d\n", restoration_outcome);
            if (!restoration_outcome)
                CHECK(active->recovered && active->recovery_proven && active->outcome != 0);
        }
        json_t *stop = json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:{s:s}}", "version", "major", 1,
                                 "minor", 0, "requestId", "stop", "method", "deployment.stop",
                                 "idempotencyKey", cycle ? "retained-stop-actor" : "stop-actor",
                                 "params", "deploymentId", "one");
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
        for (unsigned i = 0; i < 200 && descriptor_count() != descriptors_before; i++) {
            gstd_log_capture_reap();
            struct timespec delay = {0, 10000000};
            nanosleep(&delay, NULL);
        }
        CHECK(descriptor_count() == descriptors_before);
        CHECK(!restoration_outcome);
        puts("{\"deploymentActor\":\"ready\",\"stop\":\"observed\",\"volumes\":\"retained\","
             "\"scope\":"
             "\"private actor regression; public evidence separate\"}");
        json_decref(stop);
    }
    gstd_runtime_settings_clear(&settings);
    close(environment.broker_fd);
    gstd_filesystem_reply_close(&initialized);
    gstd_deployment_target_clear(&target);
    json_decref(message);
    json_decref(manifest);
    json_decref(components);
    close(source);
    close(root);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    return 0;
}
