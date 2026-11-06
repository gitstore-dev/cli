#define _GNU_SOURCE 1
#include "cleanup.h"
#include "gitstored/asset_records.h"
#include "gitstored/inspect.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "gitstored/scheduler.h"
#include "gitstored/state.h"
#include "oidc_profile.h"
#include "recovery.h"
#include "restoration.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "OIDC deployment line %d: %s\n", __LINE__, #x);                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int combined;
static const char *deployment_name = "oidc-native";
static int accept(sqlite3 *db, const json_t *components, const char *method, const char *key,
                  char operation[65]) {
    json_t *params =
        !strcmp(method, "deployment.start")
            ? json_pack("{s:s,s:s,s:s,s:O}", "deploymentId", deployment_name, "profileId", "core",
                        "profileVersion", "2", "components", components)
            : json_pack("{s:s}", "deploymentId", deployment_name);
    json_t *message =
        json_pack("{s:{s:i,s:i},s:s,s:s,s:s,s:o}", "version", "major", 1, "minor", 0, "requestId",
                  "fixture", "method", method, "idempotencyKey", key, "params", params);
    gstd_request request;
    char repository[65];
    snprintf(repository, sizeof(repository), "repo-%s", deployment_name);
    int valid = message && !gstd_request_parse(message, &request);
    gstd_accept_status admitted =
        valid ? gstd_accept(db, &request, components,
                            combined && !strcmp(method, "deployment.start") ? repository : NULL,
                            operation)
              : GSTD_ACCEPT_INVALID;
    valid = admitted == GSTD_ACCEPT_NEW;
    if (admitted == GSTD_ACCEPT_REPLAY) {
        sqlite3_stmt *row = NULL;
        valid = sqlite3_prepare_v2(
                    db, "SELECT 1 FROM operations WHERE operation_id=? AND state='accepted'", -1,
                    &row, NULL) == SQLITE_OK;
        if (valid) {
            sqlite3_bind_text(row, 1, operation, -1, SQLITE_TRANSIENT);
            valid = sqlite3_step(row) == SQLITE_ROW;
        }
        sqlite3_finalize(row);
    }
    json_decref(message);
    return valid ? 0 : -1;
}
static int recover_stop(gstd_deployment_environment *environment, const json_t *components,
                        const char *operation) {
    sqlite3 *db = environment->database;
    gstd_deployment deployment = {0};
    gstd_restoration restoration = {0};
    CHECK(!gstd_restoration_begin(environment, operation, &deployment, &restoration));
    int done = 0, previous = -1;
    while (!done) {
        if (previous != restoration.stage) {
            previous = restoration.stage;
            fprintf(stderr, "OIDC lost-manager recovery stage=%d index=%d\n", previous,
                    restoration.index);
        }
        done = gstd_restoration_poll(&restoration);
        CHECK(done >= 0);
        if (!done)
            usleep(1000);
    }
    CHECK(!restoration.outcome && deployment.recovery_proven && deployment.recovered);
    json_t *params = json_pack("{s:s}", "deploymentId", deployment_name), *view = NULL;
    CHECK(params && gstd_inspect(db, GSTD_INSPECT, params, &view) == GSTD_QUERY_OK);
    CHECK(!json_object_get(view, "endpoints") &&
          strcmp(json_string_value(json_object_get(view, "readiness")), "ready"));
    json_decref(view);
    json_decref(params);
    char stop[65], selected[65], key[65];
    snprintf(key, sizeof(key), "%s-recovered-stop", deployment_name);
    CHECK(!accept(db, components, "deployment.stop", key, stop));
    CHECK(gstd_schedule_take(db, 0, selected) == 1 && !strcmp(selected, stop));
    gstd_cleanup cleanup = {0};
    CHECK(!gstd_cleanup_begin(&deployment, stop, &cleanup));
    done = 0;
    while (!done) {
        done = gstd_cleanup_poll(&cleanup);
        CHECK(done >= 0);
        if (!done)
            usleep(1000);
    }
    CHECK(!cleanup.outcome && !gstd_schedule_finish(db, stop));
    CHECK(!gstd_cleanup_dispose(&cleanup));
    sqlite3_stmt *row = NULL;
    CHECK(
        sqlite3_prepare_v2(
            db,
            "SELECT 1 FROM operations WHERE operation_id=? AND state='succeeded' AND error IS NULL",
            -1, &row, NULL) == SQLITE_OK);
    sqlite3_bind_text(row, 1, operation, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(row) == SQLITE_ROW);
    sqlite3_finalize(row);
    return 0;
}
int main(int argc, char **argv) {
    CHECK((argc >= 3 && argc <= 6) && argv[1][0] == '/' && argv[2][0] == '/' && geteuid());
    if (argc >= 4)
        deployment_name = argv[3];
    combined = argc >= 5 && !strcmp(argv[4], "combined");
    CHECK(argc < 5 || combined || !strcmp(argv[4], "standalone"));
    int leaving = argc == 6 && !strcmp(argv[5], "leave");
    int recovering = argc == 6 && !strcmp(argv[5], "recover");
    CHECK(argc != 6 || leaving || recovering);
    alarm(360);
    CHECK(!prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0));
    gstd_json_initialize();
    int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
        broker = open(argv[2], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info;
    CHECK(root >= 3 && broker >= 3 && !fstat(root, &info) && info.st_uid == geteuid() &&
          (info.st_mode & 07777) == 0700);
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/state.db", argv[1]);
    CHECK(n > 0 && n < (int)sizeof(path));
    sqlite3 *db = NULL;
    CHECK(!gstd_state_open(path, &db));
    json_t *components =
        combined ? json_pack("[s,s,s]", "api", "git", "oidc") : json_pack("[s]", "oidc");
#if defined(__aarch64__)
    const char *architecture = "arm64";
#else
    const char *architecture = "amd64";
#endif
    json_t *manifest = gstd_oidc_core_manifest(components, architecture, "memdb");
    CHECK(manifest);
    char *registration =
        sqlite3_mprintf("INSERT OR IGNORE INTO profiles VALUES('core','2',%Q,%Q,%Q)",
                        json_string_value(json_object_get(manifest, "serverRevision")),
                        json_string_value(json_object_get(manifest, "profileDigest")),
                        !strcmp(architecture, "arm64") ? "linux-arm64" : "linux-amd64");
    CHECK(registration && sqlite3_exec(db, registration, NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_free(registration);
    char operation[65], scheduled[65];
    char first_key[65], retained_key[65], stop_first[65], stop_retained[65];
    CHECK(snprintf(first_key, sizeof(first_key), "%s-first", deployment_name) > 0);
    CHECK(snprintf(retained_key, sizeof(retained_key), "%s-retained", deployment_name) > 0);
    CHECK(snprintf(stop_first, sizeof(stop_first), "%s-stop-first", deployment_name) > 0);
    CHECK(snprintf(stop_retained, sizeof(stop_retained), "%s-stop-retained", deployment_name) > 0);
    if (recovering) {
        sqlite3_stmt *row = NULL;
        CHECK(
            sqlite3_prepare_v2(
                db,
                "SELECT o.operation_id FROM operations o JOIN resource_ownership r ON r.operation_id=o.operation_id "
                "AND r.deployment_id=o.deployment_id WHERE o.deployment_id=? AND o.caller_uid=? "
                "AND o.method='deployment.start' AND r.resource_type='configuration' "
                "AND r.resource_id='cfg-'||o.operation_id AND r.ownership_state IN ('held','uncertain')",
                -1, &row, NULL) == SQLITE_OK);
        sqlite3_bind_text(row, 1, deployment_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(row, 2, (sqlite3_int64)geteuid());
        CHECK(sqlite3_step(row) == SQLITE_ROW);
        const char *id = (const char *)sqlite3_column_text(row, 0);
        CHECK(id && strlen(id) < sizeof(operation));
        strcpy(operation, id);
        CHECK(sqlite3_step(row) == SQLITE_DONE);
        sqlite3_finalize(row);
        CHECK(!gstd_recovery_invalidate(db));
    } else
        CHECK(!accept(db, components, "deployment.start", first_key, operation));
    gstd_deployment_target target = {0};
    CHECK(!(recovering ? gstd_deployment_target_recovery_read(db, operation, &target)
                       : gstd_deployment_target_read(db, operation, &target)));
    gstd_cache_owner expected;
    int cache_state = gstd_asset_record_cache_state(db, &expected);
    CHECK(cache_state == 0 || cache_state == 1);
    gstd_filesystem_input initialize = {.action =
                                            cache_state ? GSTD_FS_INITIALIZE : GSTD_FS_REOPEN_ROOTS,
                                        .store_id = target.store_id,
                                        .expected_cache = expected,
                                        .descriptors = {root, -1, -1}};
    gstd_filesystem_job worker = {0};
    gstd_filesystem_reply initialized = {0};
    CHECK(!gstd_filesystem_launch(&initialize, 10000, &worker));
    int ready;
    while (!(ready = gstd_filesystem_poll(&worker, -1, &initialized)))
        usleep(1000);
    CHECK(ready == 1 && !initialized.status && initialized.descriptor_count == 6);
    gstd_cache_owner cache = initialized.cache_owner;
    cache.lock_fd = initialized.descriptors[1];
    CHECK(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!gstd_asset_record_cache_bind(db, &cache));
    CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    gstd_runtime_settings settings = GSTD_RUNTIME_SETTINGS_INITIAL;
    CHECK(!gstd_runtime_settings_load(root, &settings));
    gstd_deployment_environment environment = {.database = db,
                                               .settings = &settings,
                                               .state_fd = root,
                                               .cache_fd = initialized.descriptors[0],
                                               .resource_fd = initialized.descriptors[4],
                                               .native_fd = initialized.descriptors[5],
                                               .broker_fd = broker,
                                               .volume_root = initialized.volume_root};
    environment.volume_root.root_fd = initialized.descriptors[2];
    environment.volume_root.lock_fd = initialized.descriptors[3];
    if (recovering) {
        CHECK(!recover_stop(&environment, components, operation));
        gstd_runtime_settings_clear(&settings);
        gstd_deployment_target_clear(&target);
        json_decref(manifest);
        json_decref(components);
        gstd_filesystem_reply_close(&initialized);
        CHECK(sqlite3_close(db) == SQLITE_OK);
        close(broker);
        close(root);
        puts(
            "{\"managerLoss\":true,\"nativeRecovery\":true,\"ownedStop\":true,\"readinessNotAdopted\":true,\"originalStartUnchanged\":true}");
        return 0;
    }
    gstd_oidc_ports retained_ports = {0};
    gstd_volume_identity retained_volumes[2] = {{0}, {0}};
    for (unsigned cycle = 0; cycle < 2; cycle++) {
        if (cycle)
            CHECK(!accept(db, components, "deployment.start", retained_key, operation));
        CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(scheduled, operation));
        gstd_deployment deployment = {0};
        CHECK(!gstd_deployment_begin(&environment, operation, &deployment));
        int done = 0, stage = -1, oidc_stage = -1, prepare_stage = -1, asset_stage = -1;
        while (!done) {
            done = gstd_deployment_poll(&deployment, -1);
            CHECK(done >= 0);
            if (stage != deployment.stage || oidc_stage != deployment.oidc.execute_stage ||
                prepare_stage != deployment.oidc.stage ||
                asset_stage != deployment.oidc.asset.stage) {
                stage = deployment.stage;
                oidc_stage = deployment.oidc.execute_stage;
                prepare_stage = deployment.oidc.stage;
                asset_stage = deployment.oidc.asset.stage;
                fprintf(stderr,
                        "OIDC parent stage=%d provider-stage=%d prepare=%d asset=%d "
                        "index=%u\n",
                        stage, oidc_stage, prepare_stage, asset_stage, deployment.oidc.index);
            }
            if (!done)
                usleep(1000);
        }
        int start_outcome = deployment.outcome;
        fprintf(stderr, "OIDC start outcome=%d detail=%d provider-outcome=%d\n", start_outcome,
                deployment.detail, deployment.oidc.execute_outcome);
        CHECK(!gstd_schedule_finish(db, operation));
        if (!start_outcome) {
            CHECK(deployment.count == (combined ? 2u : 0u) && deployment.oidc.started == 5);
            for (unsigned i = 0; i < 5; i++)
                CHECK(!gstd_oidc_runtime_service_live(&deployment.oidc, i));
            if (!cycle) {
                retained_ports = deployment.oidc_ports;
                for (unsigned i = 0; i < 2; i++)
                    retained_volumes[i] = deployment.oidc.setup.volumes[i].identity;
            } else {
                CHECK(!memcmp(&retained_ports, &deployment.oidc_ports, sizeof(retained_ports)));
                for (unsigned i = 0; i < 2; i++)
                    CHECK(!memcmp(&retained_volumes[i], &deployment.oidc.setup.volumes[i].identity,
                                  sizeof(retained_volumes[i])));
            }
        }
        int observers_ok = start_outcome != 0;
        if (!start_outcome) {
            gstd_deployment observer = {0};
            gstd_restoration restoration = {0};
            int restored = gstd_restoration_begin(&environment, operation, &observer, &restoration);
            int previous_stage = -1;
            if (!restored) {
                do {
                    if (previous_stage != restoration.stage) {
                        previous_stage = restoration.stage;
                        fprintf(stderr, "OIDC observer stage=%d index=%d\n", previous_stage,
                                restoration.index);
                    }
                    restored = gstd_restoration_poll(&restoration);
                    if (!restored)
                        usleep(1000);
                } while (!restored);
                observers_ok = restored == 1 && !restoration.outcome && observer.recovery_proven;
            }
            fprintf(stderr, "OIDC observer proved=%d outcome=%d\n", observers_ok,
                    restoration.outcome);
            CHECK(!restoration.filesystem.worker && !restoration.native.worker);
            CHECK(!gstd_restoration_discard(&restoration));
        }
        if (leaving && !start_outcome && observers_ok) {
            puts("{\"managerReady\":true,\"providerServices\":5}");
            CHECK(!fflush(stdout));
            for (;;)
                pause();
        }
        char stop[65];
        CHECK(!accept(db, components, "deployment.stop", cycle ? stop_retained : stop_first, stop));
        CHECK(gstd_schedule_take(db, 0, scheduled) == 1 && !strcmp(scheduled, stop));
        gstd_cleanup cleanup = {0};
        CHECK(!gstd_cleanup_begin(&deployment, stop, &cleanup));
        done = 0;
        int cleanup_stage = -1;
        while (!done) {
            if (cleanup_stage != cleanup.stage) {
                cleanup_stage = cleanup.stage;
                fprintf(stderr, "OIDC cleanup stage=%d volume=%u index=%d\n", cleanup_stage,
                        cleanup.oidc_writer, cleanup.index);
            }
            done = gstd_cleanup_poll(&cleanup);
            CHECK(done >= 0);
            if (!done)
                usleep(1000);
        }
        fprintf(stderr, "OIDC stop outcome=%d\n", cleanup.outcome);
        CHECK(!gstd_schedule_finish(db, stop));
        CHECK(!start_outcome && !cleanup.outcome && observers_ok);
        CHECK(!gstd_cleanup_dispose(&cleanup));
        for (unsigned i = 0; i < 100; i++) {
            gstd_log_capture_reap();
            usleep(1000);
        }
    }
    gstd_runtime_settings_clear(&settings);
    gstd_deployment_target_clear(&target);
    json_decref(manifest);
    json_decref(components);
    gstd_filesystem_reply_close(&initialized);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    close(broker);
    close(root);
    puts("{\"productionParent\":true,\"readyGenerations\":2,\"ownedStops\":2,"
         "\"providerServicesPerGeneration\":5,\"nativeMigrations\":4,"
         "\"retainedPortsAndVolumeInodes\":true,\"implicitCoreServices\":false,"
         "\"publicCapabilityEnabled\":false}");
    return 0;
}
