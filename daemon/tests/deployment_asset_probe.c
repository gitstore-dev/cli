#define _GNU_SOURCE 1
#include "deployment_assets.h"
#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
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
/* Explicit developer fixture paths only; not linked into an installed CLI. */
int main(int argc, char **argv) {
    CHECK(argc == 3 && geteuid());
    alarm(240);
    gstd_json_initialize();
    int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
        source = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(root >= 3 && source >= 3);
    char path[4096];
    CHECK(snprintf(path, sizeof(path), "%s/state.db", argv[1]) > 0);
    sqlite3 *db = NULL;
    CHECK(!gstd_state_open(path, &db));
    json_t *components = json_pack("[s,s]", "api", "git");
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
    CHECK(!gstd_deployment_resource_reserve(db, &target, "asset-api", "asset-root"));
    CHECK(!gstd_deployment_resource_reserve(db, &target, "asset-warm", "asset-root"));
    CHECK(!gstd_deployment_resource_reserve(db, &target, "asset-tampered", "asset-root"));
    CHECK(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    gstd_deployment_asset actor = {0};
    CHECK(!gstd_deployment_asset_begin(db, &target, "api", "asset-api", initialized.descriptors[0],
                                       source, UINT64_C(128) * 1024 * 1024,
                                       UINT64_C(512) * 1024 * 1024,
                                       UINT64_C(16) * 1024 * 1024 * 1024, &actor));
    while (!gstd_deployment_asset_poll(&actor, -1)) {
        struct timespec t = {0, 10000000};
        nanosleep(&t, NULL);
    }
    CHECK(!actor.outcome && actor.active_fd >= 3 && actor.proof.logical_bytes > 0);
    uint64_t inode = actor.proof.staging_inode;
    char key[65];
    memcpy(key, actor.key, 65);
    CHECK(!gstd_deployment_asset_close(&actor));
    /* Warm acquisition with no offline source and no network consent. */
    CHECK(!gstd_deployment_asset_begin(db, &target, "api", "asset-warm", initialized.descriptors[0],
                                       -1, UINT64_C(128) * 1024 * 1024, UINT64_C(512) * 1024 * 1024,
                                       UINT64_C(16) * 1024 * 1024 * 1024, &actor));
    while (!gstd_deployment_asset_poll(&actor, -1)) {
        struct timespec t = {0, 10000000};
        nanosleep(&t, NULL);
    }
    CHECK(!actor.outcome && actor.proof.staging_inode == inode && !strcmp(key, actor.key));
    /* Add an unexpected file to the actor-owned disposable cache, then require
     * a fresh warm check to reject it. Original offline source remains intact. */
    int changed = openat(actor.active_fd, "tampered",
                         O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    CHECK(changed >= 3 && write(changed, "tampered", 8) == 8 && !close(changed));
    CHECK(!gstd_deployment_asset_close(&actor));
    CHECK(!gstd_deployment_asset_begin(db, &target, "api", "asset-tampered",
                                       initialized.descriptors[0], -1, UINT64_C(128) * 1024 * 1024,
                                       UINT64_C(512) * 1024 * 1024,
                                       UINT64_C(16) * 1024 * 1024 * 1024, &actor));
    while (!gstd_deployment_asset_poll(&actor, -1)) {
        struct timespec t = {0, 10000000};
        nanosleep(&t, NULL);
    }
    CHECK(actor.outcome == -1 && actor.active_fd == -1 && !gstd_deployment_asset_close(&actor));
    printf("{\"offlineCold\":true,\"warmNoConsent\":true,\"sameInode\":true,\"tamperRejected\":"
           "true,\"assetKey\":\"%s\"}\n",
           key);
    gstd_filesystem_reply_close(&initialized);
    gstd_deployment_target_clear(&target);
    json_decref(manifest);
    json_decref(components);
    json_decref(message);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    close(source);
    close(root);
    return 0;
}
