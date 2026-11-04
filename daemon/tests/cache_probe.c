#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/asset_cache.h"
#include "gitstored/asset_records.h"
#include "gitstored/assets.h"
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "image_backend.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <limits.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
/* Test harness only. Parent alone commits SQLite; filesystem work runs in a
 * separate bounded process with no inherited DB descriptors. */
static int promote_worker(int cache, const char *name, const char *key,
                          const gstd_provision_result *proof) {
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC))
        return -1;
    int root = fcntl(cache, F_DUPFD_CLOEXEC, 16), writer = fcntl(pipes[1], F_DUPFD_CLOEXEC, 16);
    close(pipes[1]);
    if (root < 0 || writer < 0) {
        if (root >= 0)
            close(root);
        if (writer >= 0)
            close(writer);
        close(pipes[0]);
        return -1;
    }
    pid_t parent = getpid(), child = fork();
    if (!child) {
        struct rlimit core = {0, 0},
                      memory = {UINT64_C(1024) * 1024 * 1024, UINT64_C(1024) * 1024 * 1024},
                      files = {128, 128};
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent || prctl(PR_SET_DUMPABLE, 0) ||
            dup2(root, 3) < 0 || dup2(writer, 4) < 0 || setrlimit(RLIMIT_CORE, &core) ||
            setrlimit(RLIMIT_AS, &memory) || setrlimit(RLIMIT_NOFILE, &files))
            _exit(126);
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null < 0)
            _exit(126);
        for (int fd = 0; fd < 3; fd++)
            if (dup2(null, fd) < 0)
                _exit(126);
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 5u, UINT_MAX, 0u))
#endif
            for (int fd = 5; fd < 1048576; fd++)
                close(fd);
        uint32_t result = (uint32_t)gstd_asset_promote(3, name, key, proof, -1);
        _exit(write(4, &result, sizeof(result)) == sizeof(result) ? 0 : 126);
    }
    close(root);
    close(writer);
    if (child < 0) {
        close(pipes[0]);
        return -1;
    }
    struct pollfd ready = {pipes[0], POLLIN, 0};
    uint32_t result = GSTD_CACHE_UNCERTAIN;
    int observed =
        poll(&ready, 1, 60000) > 0 && read(pipes[0], &result, sizeof(result)) == sizeof(result);
    close(pipes[0]);
    if (!observed)
        kill(child, SIGKILL);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return observed && WIFEXITED(status) && !WEXITSTATUS(status) && result == GSTD_CACHE_OK ? 0
                                                                                            : -1;
}
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "cache probe failed at line %d\n", __LINE__);                          \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
#define SQL(q) CHECK(sqlite3_exec(db, q, NULL, NULL, NULL) == SQLITE_OK)
int main(int argc, char **argv) {
    if (argc != 3 && argc != 4)
        return 2;
    int result = 0, root = -1, cache = -1, stage = -1, offline = -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *statement = NULL;
    json_t *components = NULL, *manifest = NULL, *message = NULL;
    gstd_cache_owner cache_owner = {.lock_fd = -1}, expected;
    char path[4096], operation[65], key[65], name[138];
    struct utsname host;
    CHECK(!uname(&host));
    const char *architecture = !strcmp(host.machine, "aarch64")  ? "arm64"
                               : !strcmp(host.machine, "x86_64") ? "amd64"
                                                                 : "unknown";
    gstd_json_initialize();
    components = json_pack("[s]", argv[2]);
    CHECK(components);
    manifest = gstd_asset_manifest(components, architecture);
    CHECK(manifest && gstd_image_backend_available());
    root = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat owner;
    CHECK(root >= 0 && !fstat(root, &owner) && owner.st_uid == geteuid() &&
          (owner.st_mode & 07777) == 0700);
    if (argc == 4) {
        offline = open(argv[3], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        CHECK(offline >= 0);
    }
    /* A new exclusive fixture only: refuse an existing cache directory. */
    CHECK(!mkdirat(root, "cache", 0700));
    cache = openat(root, "cache", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(cache >= 0);
    CHECK(snprintf(path, sizeof(path), "%s/state.db", argv[1]) > 0 && !gstd_state_open(path, &db));
    CHECK(gstd_asset_record_cache_state(db, &expected) == 1);
    CHECK(gstd_asset_cache_lock(cache, expected.store_id, 0, 0, 1, &cache_owner) == GSTD_CACHE_OK);
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_asset_record_cache_bind(db, &cache_owner));
    SQL("COMMIT");
    char platform[64];
    snprintf(platform, sizeof(platform), "linux-%s", architecture);
    CHECK(sqlite3_prepare_v2(db, "INSERT INTO profiles VALUES('core','1',?,?,?)", -1, &statement,
                             NULL) == SQLITE_OK);
    sqlite3_bind_text(statement, 1, json_string_value(json_object_get(manifest, "serverRevision")),
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, json_string_value(json_object_get(manifest, "profileDigest")),
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, platform, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(statement);
    sqlite3_finalize(statement);
    statement = NULL;
    CHECK(step == SQLITE_DONE);
    const char request[] =
        "{\"version\":{\"major\":1,\"minor\":0},\"requestId\":\"probe\",\"method\":\"deployment."
        "start\",\"idempotencyKey\":\"cache-probe\",\"params\":{\"deploymentId\":\"cache-probe\","
        "\"profileId\":\"core\",\"profileVersion\":\"1\",\"allowProvision\":true}}";
    message = gstd_json_parse((const unsigned char *)request, sizeof(request) - 1);
    CHECK(message);
    if (offline >= 0)
        CHECK(!json_object_set_new(json_object_get(message, "params"), "allowProvision",
                                   json_false()));
    gstd_request parsed;
    CHECK(!gstd_request_parse(message, &parsed));
    CHECK(gstd_accept(db, &parsed, components, "cache-probe-repository", operation) ==
          GSTD_ACCEPT_NEW);
    SQL("BEGIN IMMEDIATE");
    CHECK(sqlite3_prepare_v2(db,
                             "INSERT INTO resource_ownership "
                             "VALUES('cache-probe-asset','asset-root','cache-probe',?,'reserved',"
                             "NULL,'held','2026-11-03T00:00:00Z')",
                             -1, &statement, NULL) == SQLITE_OK);
    sqlite3_bind_text(statement, 1, operation, -1, SQLITE_TRANSIENT);
    step = sqlite3_step(statement);
    sqlite3_finalize(statement);
    statement = NULL;
    CHECK(step == SQLITE_DONE);
    const uint64_t download = UINT64_C(128) * 1024 * 1024, merge = UINT64_C(512) * 1024 * 1024;
    gstd_asset_record_status reservation =
        offline >= 0
            ? gstd_asset_record_begin_offline(db, manifest, offline, argv[2], operation,
                                              "cache-probe-asset", download, merge,
                                              UINT64_C(4) * 1024 * 1024 * 1024, key, name)
            : gstd_asset_record_begin(db, manifest, argv[2], operation, "cache-probe-asset",
                                      download, merge, UINT64_C(4) * 1024 * 1024 * 1024, key, name);
    CHECK(reservation == GSTD_ASSET_RECORD_NEW);
    SQL("COMMIT");
    CHECK(!mkdirat(cache, name, 0700));
    stage = openat(cache, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(stage >= 0);
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_asset_record_identity(db, key, operation, stage));
    SQL("COMMIT");
    gstd_image_auth anonymous = {0};
    gstd_provision_result proof = {0};
    CHECK(!gstd_asset_record_source(db, key, operation, offline));
    gstd_provision_status acquisition =
        offline >= 0 ? gstd_provision_import(manifest, argv[2], offline, stage, download, merge,
                                             60000, -1, &proof)
                     : gstd_provision_stage(manifest, argv[2], stage, &anonymous, download, merge,
                                            60000, -1, &proof);
    CHECK(acquisition == GSTD_PROVISION_OK);
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_asset_record_verified(db, key, operation, &proof));
    SQL("COMMIT");
    CHECK(!promote_worker(cache, name, key, &proof));
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_asset_record_active(db, key, operation, cache));
    SQL("COMMIT");
    uint64_t references = 0;
    CHECK(!gstd_asset_record_refcount(db, key, &references) && references == 1);
    CHECK(sqlite3_exec(db, "DELETE FROM assets", NULL, NULL, NULL) != SQLITE_OK);
    printf("{\"scope\":\"image cache only; no service "
           "execution\",\"component\":\"%s\",\"assetKey\":\"%s\",\"state\":\"active\","
           "\"references\":%" PRIu64 ",\"downloadedBytes\":%" PRIu64 ",\"mergedBytes\":%" PRIu64
           ",\"logicalBytes\":%" PRIu64 ",\"treeDigest\":\"%s\"}\n",
           argv[2], key, references, proof.downloaded_bytes, proof.merged_bytes,
           proof.logical_bytes, proof.tree_digest);
done:
    gstd_asset_cache_unlock(&cache_owner);
    if (offline >= 0)
        close(offline);
    sqlite3_finalize(statement);
    if (db)
        sqlite3_close(db);
    if (stage >= 0)
        close(stage);
    if (cache >= 0)
        close(cache);
    if (root >= 0)
        close(root);
    json_decref(message);
    json_decref(manifest);
    json_decref(components);
    return result;
}
#else
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 2;
}
#endif
