#define _GNU_SOURCE 1
#include "gitstored/operations.h"
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "support.h"
#include "volume_records.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d\n", __LINE__);                                                \
            result = 1;                                                                            \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
#define SQL(x) CHECK(sqlite3_exec(db, x, NULL, NULL, NULL) == SQLITE_OK)
int main(void) {
    int result = 0, root_fd = -1;
    char *root = gstd_test_dir(), path[1024], operation[65], producer[65];
    sqlite3 *db = NULL;
    json_t *message = NULL, *components = NULL;
    gstd_volume_root owner = {.lock_fd = -1};
    gstd_volume_handle handle = {.data_fd = -1, .writer_fd = -1};
    gstd_volume_identity expected;
    gstd_json_initialize();
    CHECK(root);
    snprintf(path, sizeof(path), "%s/state.db", root);
    CHECK(!gstd_state_open(path, &db));
    CHECK(gstd_volume_record_begin(db, &owner, "repositories", "operation", producer, &expected) ==
          -1);
    SQL("INSERT INTO profiles VALUES('core','1','source','digest','linux-arm64')");
    const char request_text[] =
        "{\"version\":{\"major\":1,\"minor\":0},\"requestId\":\"r\",\"method\":\"deployment."
        "start\",\"idempotencyKey\":\"volume-start\",\"params\":{\"deploymentId\":\"one\","
        "\"profileId\":\"core\",\"profileVersion\":\"1\"}}";
    message = gstd_json_parse((const unsigned char *)request_text, sizeof(request_text) - 1);
    components = json_pack("[s]", "git");
    gstd_request request;
    CHECK(message && components && !gstd_request_parse(message, &request));
    CHECK(gstd_accept(db, &request, components, "repositories", operation) == GSTD_ACCEPT_NEW);
#ifdef __linux__
    sqlite3_stmt *query = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT store_id FROM query_meta", -1, &query, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(query) == SQLITE_ROW);
    char store[33];
    snprintf(store, sizeof(store), "%s", sqlite3_column_text(query, 0));
    sqlite3_finalize(query);
    snprintf(path, sizeof(path), "%s/volumes", root);
    CHECK(!mkdir(path, 0700));
    root_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(root_fd >= 3 && gstd_volume_root_lock(root_fd, store, &owner) == GSTD_VOLUME_OK);
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_volume_record_begin(db, &owner, "repositories", "foreign", producer, &expected) ==
          -1);
    SQL("ROLLBACK");
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_volume_record_begin(db, &owner, "repositories", operation, producer, &expected) ==
          0);
    CHECK(!strcmp(producer, operation) && !expected.data_inode);
    SQL("COMMIT");
    CHECK(gstd_volume_acquire(root_fd, &owner, "repositories", producer, 1, NULL, &handle) ==
          GSTD_VOLUME_OK);
    CHECK(gstd_volume_record_ready(db, &owner, "repositories", operation, &handle) == -1);
    SQL("BEGIN IMMEDIATE");
    CHECK(!gstd_volume_record_ready(db, &owner, "repositories", operation, &handle));
    CHECK(!gstd_volume_record_ready(db, &owner, "repositories", operation, &handle));
    SQL("COMMIT");
    char saved_volume[65], saved_producer[65];
    gstd_volume_identity saved_identity;
    CHECK(!gstd_volume_record_recovery_read(db, &owner, operation, saved_volume, saved_producer,
                                            &saved_identity));
    CHECK(!strcmp(saved_volume, "repositories") && !strcmp(saved_producer, operation) &&
          saved_identity.data_inode == handle.identity.data_inode &&
          saved_identity.writer_inode == handle.identity.writer_inode);
    SQL("BEGIN IMMEDIATE;UPDATE resource_ownership SET ownership_state='released'");
    CHECK(gstd_volume_record_recovery_read(db, &owner, operation, saved_volume, saved_producer,
                                           &saved_identity) == -1);
    SQL("ROLLBACK");

    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_volume_record_begin(db, &owner, "repositories", operation, producer, &expected) ==
          1);
    CHECK(expected.data_inode == handle.identity.data_inode &&
          expected.writer_inode == handle.identity.writer_inode);
    SQL("COMMIT");
    CHECK(sqlite3_exec(db, "UPDATE repository_volumes SET allocated=0", NULL, NULL, NULL) !=
          SQLITE_OK);
    CHECK(sqlite3_exec(db, "DELETE FROM repository_volumes", NULL, NULL, NULL) != SQLITE_OK);
    gstd_volume_root changed = owner;
    changed.inode++;
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_volume_record_begin(db, &changed, "repositories", operation, producer, &expected) ==
          -1);
    SQL("ROLLBACK");
    CHECK(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    snprintf(path, sizeof(path), "%s/state.db", root);
    CHECK(!gstd_state_open(path, &db));
    SQL("BEGIN IMMEDIATE");
    CHECK(gstd_volume_record_begin(db, &owner, "repositories", operation, producer, &expected) ==
          1);
    SQL("COMMIT");
#endif
done:
    gstd_volume_handle_close(&handle);
    gstd_volume_root_close(&owner);
    if (root_fd >= 0)
        close(root_fd);
    json_decref(message);
    json_decref(components);
    if (db)
        sqlite3_close(db);
    if (root && gstd_test_remove(root))
        result = 1;
    return result;
}
