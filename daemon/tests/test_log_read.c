#define _POSIX_C_SOURCE 200809L
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "log_read.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "log read line %d: %s\n", __LINE__, #x);                               \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int finish(sqlite3 *db, gstd_log_read *job) {
    for (unsigned i = 0; i < 6000; i++) {
        int result = gstd_log_read_poll(db, job);
        if (result)
            return result;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    return -1;
}
int main(void) {
    gstd_json_initialize();
    int result = 1, state = -1, directory = -1;
    char *root = gstd_test_dir(), path[512], cursor[65];
    const char *store = "0123456789abcdef0123456789abcdef";
    sqlite3 *db = NULL;
    gstd_logs *logs = NULL;
    json_t *payload = json_pack("{s:s}", "text", "safe retained line");
    gstd_log_read first = GSTD_LOG_READ_INITIAL, second = GSTD_LOG_READ_INITIAL,
                  third = GSTD_LOG_READ_INITIAL;
    CHECK(root && payload);
    snprintf(path, sizeof(path), "%s/state.db", root);
    CHECK(!gstd_state_open(path, &db));
    CHECK(sqlite3_exec(db,
                       "INSERT INTO profiles VALUES('core','1','source','digest','linux-arm64');"
                       "INSERT INTO deployments "
                       "VALUES('observed','core','1','running',1,0,'pending','normal','released','"
                       "2000-01-01T00:00:00Z',NULL)",
                       NULL, NULL, NULL) == SQLITE_OK);
    state = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(state >= 3);
    gstd_log_read_input input = {.state_fd = state,
                                 .store_id = store,
                                 .subscription_id = "reader-1",
                                 .deployment_id = "observed",
                                 .kind = GSTD_STREAM_LOGS,
                                 .limit = 256,
                                 .now = 100};
    CHECK(gstd_log_read_launch(db, &input, &first) == 1);
    int finished = finish(db, &first);
    if (finished != 1 || first.status != GSTD_LOG_OK)
        fprintf(stderr,
                "reader outcome finished=%d status=%d exit_ok=%d exit_status=%d abandoned=%d "
                "received=%zu expected=%zu header=%u\n",
                finished, first.status, first.exit_ok, first.exit_status, first.abandoned,
                first.received, first.expected, first.header[0]);
    CHECK(finished == 1 && first.status == GSTD_LOG_OK && !json_array_size(first.records));
    gstd_log_read_close(&first);
    char line[1025];
    memset(line, 'x', sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
    CHECK(!json_object_set_new(payload, "text", json_string(line)));
    directory = gstd_logs_directory(state, store, 1);
    CHECK(directory >= 3 && gstd_logs_open_at(directory, NULL, 1, &logs) == GSTD_LOG_OK);
    for (unsigned i = 0; i < 256; i++)
        CHECK(gstd_logs_append(logs, "observed", "api", GSTD_STREAM_LOGS, payload, 100) ==
              GSTD_LOG_OK);
    CHECK(gstd_log_read_launch(db, &input, &first) == 1);
    input.subscription_id = "reader-2";
    CHECK(gstd_log_read_launch(db, &input, &second) == 1);
    input.subscription_id = "reader-3";
    CHECK(gstd_log_read_launch(db, &input, &third) == 0);
    gstd_log_read_abandon(&second);
    /* Abandon is observation-only and cannot free a live process lease. */
    CHECK(gstd_log_read_launch(db, &input, &third) == 0);
    CHECK(finish(db, &second) == 1 && !second.records);
    gstd_log_read_close(&second);
    CHECK(finish(db, &first) == 1 && first.status == GSTD_LOG_OK &&
          json_array_size(first.records) == 256);
    snprintf(cursor, sizeof(cursor), "%s",
             json_string_value(json_object_get(json_array_get(first.records, 255), "cursor")));
    CHECK(!strcmp(json_string_value(json_object_get(json_array_get(first.records, 0), "text")),
                  line));
    gstd_log_read_close(&first);
    input.cursor = cursor;
    CHECK(gstd_log_read_launch(db, &input, &third) == 1);
    CHECK(finish(db, &third) == 1 && third.status == GSTD_LOG_OK &&
          !json_array_size(third.records));
    gstd_log_read_close(&third);
    input.cursor = "log:old-index:1";
    CHECK(gstd_log_read_launch(db, &input, &first) == 1);
    CHECK(finish(db, &first) == 1 && first.status == GSTD_LOG_CURSOR_EXPIRED && !first.records);
    gstd_log_read_close(&first);
    result = 0;
done:;
    gstd_log_read *jobs[] = {&first, &second, &third};
    for (unsigned i = 0; i < 3; i++) {
        if (jobs[i]->worker && !jobs[i]->complete) {
            gstd_log_read_abandon(jobs[i]);
            (void)finish(db, jobs[i]);
        }
        gstd_log_read_close(jobs[i]);
    }
    json_decref(payload);
    gstd_logs_close(logs);
    if (directory >= 0)
        close(directory);
    if (state >= 0)
        close(state);
    if (db)
        sqlite3_close(db);
    if (root)
        gstd_test_remove(root);
    return result;
}
