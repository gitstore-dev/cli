#define _POSIX_C_SOURCE 200809L
#include "gitstored/protocol.h"
#include "logs.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "log directory line %d: %s\n", __LINE__, #x);                          \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
int main(void) {
    gstd_json_initialize();
    int result = 1, state = -1, directory = -1;
    char *root = gstd_test_dir();
    const char *store = "0123456789abcdef0123456789abcdef";
    gstd_logs *writer = NULL, *reader = NULL;
    json_t *payload = json_pack("{s:s}", "text", "retained line"), *page = NULL;
    gstd_log_limits limits = {512, 1024, 2048, 8, 16, 60};
    CHECK(root && payload);
    state = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(state >= 3);
    CHECK(gstd_logs_directory(state, store, 0) == -1);
    struct stat info;
    CHECK(fstatat(state, "logs", &info, AT_SYMLINK_NOFOLLOW) == -1);
    directory = gstd_logs_directory(state, store, 1);
    CHECK(directory >= 3);
    CHECK(gstd_logs_directory(state, "ffffffffffffffffffffffffffffffff", 0) == -1);
    CHECK(gstd_logs_open_at(directory, &limits, 0, &reader) == GSTD_LOG_IO && !reader);
    CHECK(fstatat(directory, "segments.db", &info, AT_SYMLINK_NOFOLLOW) == -1);
    CHECK(gstd_logs_open_at(directory, &limits, 1, &writer) == GSTD_LOG_OK);
    CHECK(gstd_logs_append(writer, "observed", "api", GSTD_STREAM_LOGS, payload, 100) ==
          GSTD_LOG_OK);
    CHECK(gstd_logs_open_at(directory, &limits, 0, &reader) == GSTD_LOG_OK);
    CHECK(gstd_logs_replay(reader, "observed", NULL, GSTD_STREAM_LOGS, NULL, 256, 100, &page) ==
              GSTD_LOG_OK &&
          json_array_size(page) == 1);
    char cursor[65];
    snprintf(cursor, sizeof(cursor), "%s",
             json_string_value(json_object_get(json_array_get(page, 0), "cursor")));
    json_decref(page);
    page = NULL;
    CHECK(gstd_logs_append(reader, "observed", "api", GSTD_STREAM_LOGS, payload, 101) !=
          GSTD_LOG_OK);
    CHECK(gstd_logs_prune(reader, 161) != GSTD_LOG_OK);
    CHECK(gstd_logs_replay(reader, "observed", NULL, GSTD_STREAM_LOGS, cursor, 256, 161, &page) ==
              GSTD_LOG_CURSOR_EXPIRED &&
          !page);
    CHECK(gstd_logs_replay(reader, "observed", NULL, GSTD_STREAM_LOGS, NULL, 256, 161, &page) ==
              GSTD_LOG_OK &&
          !json_array_size(page));
    /* Reader age filtering must not physically delete collector-owned history. */
    CHECK(gstd_logs_retained_bytes(writer) > 0);
    result = 0;
done:
    json_decref(payload);
    json_decref(page);
    gstd_logs_close(reader);
    gstd_logs_close(writer);
    if (directory >= 0)
        close(directory);
    if (state >= 0)
        close(state);
    if (root)
        gstd_test_remove(root);
    return result;
}
