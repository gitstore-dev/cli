#define _GNU_SOURCE 1
#include "filesystem_job.h"
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "oidc_profile.h"
#include "support.h"
#include "volume_records.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "OIDC volume line %d\n", __LINE__);                                    \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
#define SQL(s) CHECK(sqlite3_exec(db, s, NULL, NULL, NULL) == SQLITE_OK)
#ifdef __linux__
static int volume_worker(const gstd_filesystem_input *input, gstd_volume_handle *handle) {
    gstd_filesystem_job job = {0};
    if (gstd_filesystem_launch(input, 5000, &job))
        return -1;
    gstd_filesystem_reply reply;
    int ready = 0;
    for (int i = 0; i < 6000 && !(ready = gstd_filesystem_poll(&job, -1, &reply)); i++)
        usleep(1000);
    if (ready != 1)
        return -1;
    int observed = input->action == GSTD_FS_OBSERVE_OIDC_VOLUME;
    if (reply.status || reply.detail != GSTD_VOLUME_OK ||
        reply.descriptor_count != (observed ? 1u : 2u)) {
        gstd_filesystem_reply_close(&reply);
        return -1;
    }
    *handle = (gstd_volume_handle){.data_fd = reply.descriptors[0],
                                   .writer_fd = observed ? -1 : reply.descriptors[1],
                                   .identity = reply.volume};
    reply.descriptor_count = 0;
    return 0;
}
static int operation(sqlite3 *db, const char *id, int first) {
    char *sql = sqlite3_mprintf(
        "%s INSERT INTO operations VALUES('%q',%lld,'deployment.start','provider-test',NULL,"
        "printf('%%064d',0),'{\"components\":[\"oidc\"]}','2026-11-05T00:00:00Z',"
        "'2026-11-05T00:00:00Z','running','normal',NULL,NULL);"
        "UPDATE deployments SET "
        "last_operation_id='%q',desired_state='running',writer_ownership='held',"
        "generation=generation+1 WHERE deployment_id='provider-test';",
        first ? ""
              : "UPDATE operations SET state='succeeded',result='{}' WHERE state='running';"
                "UPDATE resource_ownership SET "
                "ownership_state='released',lifecycle_stage='retained';",
        id, (sqlite3_int64)geteuid(), id);
    int ok = sql && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_free(sql);
    return ok ? 0 : -1;
}
#endif
int main(void) {
    gstd_json_initialize();
    int result = 0, root_fd = -1, release[2] = {-1, -1};
    pid_t child = -1;
    char *root = gstd_test_dir(), path[512];
    sqlite3 *db = NULL;
    gstd_volume_root owner = {.lock_fd = -1};
    json_t *manifest = NULL;
    gstd_volume_handle handles[2] = {{.data_fd = -1, .writer_fd = -1},
                                     {.data_fd = -1, .writer_fd = -1}},
                       other = {.data_fd = -1, .writer_fd = -1};
    CHECK(root);
    snprintf(path, sizeof(path), "%s/state.db", root);
    CHECK(!gstd_state_open(path, &db));
#ifdef __linux__
    SQL("INSERT INTO profiles VALUES('core','2','controlled','controlled','linux-arm64');"
        "INSERT INTO deployments VALUES('provider-test','core','2','running',1,0,'pending',"
        "'normal','held','2026-11-05T00:00:00Z',NULL)");
    json_t *selection = json_pack("[s]", "oidc");
#if defined(__aarch64__)
    const char *architecture = "arm64";
#else
    const char *architecture = "amd64";
#endif
    manifest = selection ? gstd_oidc_core_manifest(selection, architecture, "memdb") : NULL;
    json_decref(selection);
    CHECK(manifest);
    CHECK(!operation(db, "first", 1));
    snprintf(path, sizeof(path), "%s/volumes", root);
    CHECK(!mkdir(path, 0700));
    root_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(root_fd >= 3);
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT store_id FROM query_meta", -1, &s, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(s) == SQLITE_ROW);
    char store[33];
    snprintf(store, sizeof(store), "%s", sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    CHECK(gstd_volume_root_lock(root_fd, store, &owner) == GSTD_VOLUME_OK);
    char ids[2][65], producer[65], recovered[65];
    gstd_volume_identity expected, receipts[2];
    CHECK(gstd_oidc_volume_reserve(db, GSTD_OIDC_VOLUME_HYDRA, "first", ids[0]) ==
          GSTD_ACCEPT_INVALID);
    SQL("BEGIN IMMEDIATE");
    for (int role = 0; role < 2; role++) {
        CHECK(gstd_oidc_volume_reserve(db, (gstd_oidc_volume_role)role, "first", ids[role]) ==
              GSTD_ACCEPT_NEW);
        char replay[65];
        CHECK(gstd_oidc_volume_reserve(db, (gstd_oidc_volume_role)role, "first", replay) ==
                  GSTD_ACCEPT_REPLAY &&
              !strcmp(replay, ids[role]));
        CHECK(gstd_oidc_volume_record_begin(db, &owner, (gstd_oidc_volume_role)role, ids[role],
                                            "first", producer, &expected) == 0 &&
              !strcmp(producer, "first"));
    }
    CHECK(strcmp(ids[0], ids[1]));
    SQL("COMMIT");
    for (int role = 0; role < 2; role++) {
        gstd_filesystem_input input = {.action = GSTD_FS_OIDC_VOLUME,
                                       .manifest = manifest,
                                       .volume_id = ids[role],
                                       .producer_id = "first",
                                       .descriptors = {root_fd, owner.lock_fd, -1},
                                       .volume_root = owner,
                                       .oidc_volume_role = (gstd_oidc_volume_role)role};
        CHECK(!volume_worker(&input, &handles[role]));
        receipts[role] = handles[role].identity;
        SQL("BEGIN IMMEDIATE");
        CHECK(gstd_oidc_volume_record_ready(db, &owner, (gstd_oidc_volume_role)(1 - role),
                                            ids[role], "first", &handles[role]) == -1);
        if (role == GSTD_OIDC_VOLUME_KRATOS)
            CHECK(gstd_oidc_volume_record_ready(db, &owner, GSTD_OIDC_VOLUME_KRATOS, ids[role],
                                                "first", &handles[GSTD_OIDC_VOLUME_HYDRA]) == -1);
        CHECK(gstd_oidc_volume_record_ready(db, &owner, (gstd_oidc_volume_role)role, ids[role],
                                            "first", &handles[role]) == 0);
        CHECK(gstd_oidc_volume_record_ready(db, &owner, (gstd_oidc_volume_role)role, ids[role],
                                            "first", &handles[role]) == 0);
        SQL("COMMIT");
    }
    CHECK(sqlite3_exec(db, "DELETE FROM oidc_provider_volumes", NULL, NULL, NULL) != SQLITE_OK);
    CHECK(sqlite3_exec(db, "UPDATE oidc_provider_volumes SET allocated=0", NULL, NULL, NULL) !=
          SQLITE_OK);
    CHECK(!pipe(release));
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(release[1]);
        char byte;
        int n = read(release[0], &byte, 1);
        gstd_volume_handle_close(&handles[0]);
        gstd_volume_handle_close(&handles[1]);
        _exit(n == 1 && byte == 'x' ? 0 : 1);
    }
    close(release[0]);
    release[0] = -1;
    gstd_volume_handle_close(&handles[0]);
    gstd_volume_handle_close(&handles[1]);
    SQL("BEGIN IMMEDIATE");
    CHECK(!operation(db, "second", 0));
    for (int role = 0; role < 2; role++) {
        char rebound[65];
        CHECK(gstd_oidc_volume_reserve(db, (gstd_oidc_volume_role)role, "second", rebound) ==
                  GSTD_ACCEPT_NEW &&
              !strcmp(rebound, ids[role]));
        CHECK(gstd_oidc_volume_record_begin(db, &owner, (gstd_oidc_volume_role)role, ids[role],
                                            "second", producer, &expected) == 1 &&
              !strcmp(producer, "first") && !memcmp(&expected, &receipts[role], sizeof(expected)));
    }
    SQL("COMMIT");
    for (int role = 0; role < 2; role++) {
        CHECK(gstd_oidc_volume_acquire(root_fd, &owner, (gstd_oidc_volume_role)role, ids[role],
                                       "first", 0, &receipts[role], &other) == GSTD_VOLUME_HELD);
        CHECK(gstd_oidc_volume_record_recovery_read(db, &owner, (gstd_oidc_volume_role)role,
                                                    "second", recovered, producer,
                                                    &expected) == 0 &&
              !strcmp(recovered, ids[role]) && !strcmp(producer, "first"));
        gstd_filesystem_input observation = {.action = GSTD_FS_OBSERVE_OIDC_VOLUME,
                                             .manifest = manifest,
                                             .volume_id = recovered,
                                             .producer_id = producer,
                                             .volume_expected = expected,
                                             .volume_existing = 1,
                                             .descriptors = {root_fd, owner.lock_fd, -1},
                                             .volume_root = owner,
                                             .oidc_volume_role = (gstd_oidc_volume_role)role};
        CHECK(!volume_worker(&observation, &other) && other.writer_fd == -1);
        gstd_volume_handle_close(&other);
    }
    CHECK(write(release[1], "x", 1) == 1);
    close(release[1]);
    release[1] = -1;
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    child = -1;
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    for (int role = 0; role < 2; role++) {
        CHECK(gstd_oidc_volume_acquire(root_fd, &owner, (gstd_oidc_volume_role)role, ids[role],
                                       "first", 0, &receipts[role], &other) == GSTD_VOLUME_OK);
        gstd_volume_handle_close(&other);
    }
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_oidc_volume_record_begin(db, &owner, GSTD_OIDC_VOLUME_HYDRA, ids[0], "first",
                                        producer, &expected) == -1);
    SQL("UPDATE deployments SET recovery='uncertain'");
    CHECK(gstd_oidc_volume_reserve(db, GSTD_OIDC_VOLUME_HYDRA, "second", recovered) ==
          GSTD_ACCEPT_NOT_FOUND);
    SQL("ROLLBACK");
#endif
done:
    json_decref(manifest);
    if (release[0] >= 0)
        close(release[0]);
    if (release[1] >= 0)
        close(release[1]);
    if (child > 0)
        waitpid(child, NULL, 0);
    gstd_volume_handle_close(&handles[0]);
    gstd_volume_handle_close(&handles[1]);
    gstd_volume_handle_close(&other);
    gstd_volume_root_close(&owner);
    if (root_fd >= 0)
        close(root_fd);
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
