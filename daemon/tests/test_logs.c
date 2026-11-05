#define _POSIX_C_SOURCE 200809L
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "logs.h"
#include "redaction.h"
#include "support.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "logs line %d: %s\n", __LINE__, #x);                                   \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
typedef struct {
    gstd_logs *logs;
} sink;
static int ingest(void *context, const char *text, size_t length) {
    sink *target = context;
    json_t *payload = json_object(), *value = json_stringn(text, length);
    int ok = payload && value && !json_object_set(payload, "text", value) &&
             gstd_logs_append(target->logs, "redacted", "api", GSTD_STREAM_LOGS, payload, 200) ==
                 GSTD_LOG_OK;
    json_decref(payload);
    json_decref(value);
    return ok ? 0 : -1;
}
static int count(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        value = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return value;
}
static int contains(const unsigned char *bytes, size_t length, const char *needle) {
    size_t size = strlen(needle);
    for (size_t i = 0; i + size <= length; i++)
        if (!memcmp(bytes + i, needle, size))
            return 1;
    return 0;
}
int main(void) {
    gstd_json_initialize();
    int result = 1, fd = -1;
    char *root = gstd_test_dir(), path[512], metadata_path[512], alias[512], cursor[65] = {0};
    gstd_logs *logs = NULL, *foreign = NULL;
    sqlite3 *metadata = NULL;
    json_t *payload = NULL, *page = NULL;
    gstd_redactor *redactor = NULL;
    CHECK(root);
    snprintf(path, sizeof(path), "%s/logs.db", root);
    snprintf(metadata_path, sizeof(metadata_path), "%s/metadata.db", root);
    snprintf(alias, sizeof(alias), "%s/alias.db", root);
    CHECK(!gstd_state_open(metadata_path, &metadata));
    CHECK(sqlite3_exec(metadata,
                       "INSERT INTO profiles VALUES('kept','1','source','digest','linux-arm64')",
                       NULL, NULL, NULL) == SQLITE_OK);
    gstd_log_limits limits = {512, 1024, 2048, 8, 16, 60};
    CHECK(gstd_logs_open(path, &limits, &logs) == GSTD_LOG_OK);
    CHECK(gstd_logs_open(metadata_path, &limits, &foreign) == GSTD_LOG_IO && !foreign);
    CHECK(count(metadata, "SELECT count(*) FROM profiles WHERE profile_id='kept'") == 1);
    CHECK(!symlink(path, alias));
    CHECK(gstd_logs_open(alias, &limits, &foreign) == GSTD_LOG_IO && !foreign);
    char text[201];
    memset(text, 'x', 200);
    text[200] = 0;
    payload = json_pack("{s:s}", "text", text);
    CHECK(payload);
    CHECK(gstd_logs_append(logs, "one", "api", GSTD_STREAM_LOGS, payload, 100) == GSTD_LOG_OK);
    CHECK(gstd_logs_replay(logs, "one", NULL, GSTD_STREAM_LOGS, NULL, 256, 100, &page) ==
              GSTD_LOG_OK &&
          json_array_size(page) == 1);
    const char *value = json_string_value(json_object_get(json_array_get(page, 0), "cursor"));
    CHECK(value && strlen(value) < sizeof(cursor));
    memcpy(cursor, value, strlen(value) + 1);
    CHECK(json_object_get(json_array_get(page, 0), "observedAt"));
    json_decref(page);
    page = NULL;
    CHECK(gstd_logs_replay(logs, "one", NULL, GSTD_STREAM_LOGS, cursor, 256, 100, &page) ==
              GSTD_LOG_OK &&
          !json_array_size(page));
    json_decref(page);
    page = NULL;
    CHECK(gstd_logs_replay(logs, "foreign", NULL, GSTD_STREAM_LOGS, cursor, 256, 100, &page) ==
              GSTD_LOG_CURSOR_EXPIRED &&
          !page);
    gstd_logs_close(logs);
    logs = NULL;
    CHECK(gstd_logs_open(path, &limits, &logs) == GSTD_LOG_OK);
    CHECK(gstd_logs_replay(logs, "one", NULL, GSTD_STREAM_LOGS, cursor, 256, 100, &page) ==
          GSTD_LOG_OK);
    json_decref(page);
    page = NULL;
    for (unsigned i = 0; i < 3; i++)
        CHECK(gstd_logs_append(logs, "one", "api", GSTD_STREAM_LOGS, payload, 101 + i) ==
              GSTD_LOG_OK);
    CHECK(gstd_logs_retained_bytes(logs) <= 1024);
    CHECK(gstd_logs_replay(logs, "one", NULL, GSTD_STREAM_LOGS, cursor, 256, 103, &page) ==
              GSTD_LOG_CURSOR_EXPIRED &&
          !page);
    CHECK(gstd_logs_replay(logs, "one", NULL, GSTD_STREAM_LOGS, NULL, 256, 103, &page) ==
              GSTD_LOG_OK &&
          json_array_size(page) == 3);
    json_decref(page);
    page = NULL;
    for (unsigned i = 0; i < 20; i++) {
        char deployment[32];
        snprintf(deployment, sizeof(deployment), "other-%u", i);
        CHECK(gstd_logs_append(logs, deployment, "git", GSTD_STREAM_LOGS, payload, 104) ==
              GSTD_LOG_OK);
        CHECK(gstd_logs_retained_bytes(logs) <= 2048);
    }
    CHECK(count(metadata, "SELECT count(*) FROM profiles WHERE profile_id='kept'") == 1);
    CHECK(gstd_logs_prune(logs, 165) == GSTD_LOG_OK && !gstd_logs_retained_bytes(logs));
    CHECK(gstd_logs_replay(logs, "one", NULL, GSTD_STREAM_LOGS, NULL, 256, 165, &page) ==
              GSTD_LOG_OK &&
          !json_array_size(page));
    json_decref(page);
    page = NULL;
    json_object_set_new(payload, "cursor", json_string("forged"));
    CHECK(gstd_logs_append(logs, "one", "api", GSTD_STREAM_LOGS, payload, 165) == GSTD_LOG_INVALID);
    json_object_del(payload, "cursor");
    CHECK(gstd_logs_replay(logs, "one", NULL, GSTD_STREAM_LOGS, "log:old-index:1", 256, 165,
                           &page) == GSTD_LOG_CURSOR_EXPIRED);
    const char *secrets[] = {"fixture-key-never-written-123456789"};
    sink target = {.logs = logs};
    redactor = gstd_redactor_open(secrets, 1, ingest, &target);
    CHECK(redactor);
    for (size_t i = 0; i < strlen(secrets[0]); i++)
        CHECK(!gstd_redactor_feed(redactor, secrets[0] + i, 1));
    CHECK(!gstd_redactor_feed(redactor, "\n", 1) && !gstd_redactor_finish(redactor));
    CHECK(gstd_logs_replay(logs, "redacted", "api", GSTD_STREAM_LOGS, NULL, 256, 200, &page) ==
              GSTD_LOG_OK &&
          json_array_size(page) == 1);
    CHECK(
        !strcmp(json_string_value(json_object_get(json_array_get(page, 0), "text")), "[REDACTED]"));
    json_decref(page);
    page = NULL;
    gstd_redactor_close(redactor);
    redactor = NULL;
    gstd_logs_close(logs);
    logs = NULL;
    fd = open(path, O_RDONLY);
    struct stat info;
    CHECK(fd >= 0 && !fstat(fd, &info) && info.st_size > 0 && info.st_size < 1048576);
    unsigned char *bytes = malloc((size_t)info.st_size);
    CHECK(bytes);
    ssize_t received = read(fd, bytes, (size_t)info.st_size);
    int absent = received == info.st_size && !contains(bytes, (size_t)received, secrets[0]);
    free(bytes);
    CHECK(absent);
    CHECK(!chmod(path, 0644));
    CHECK(gstd_logs_open(path, &limits, &logs) == GSTD_LOG_IO && !logs);
    result = 0;
done:
    if (fd >= 0)
        close(fd);
    json_decref(payload);
    json_decref(page);
    gstd_redactor_close(redactor);
    gstd_logs_close(logs);
    gstd_logs_close(foreign);
    if (metadata)
        sqlite3_close(metadata);
    if (root)
        gstd_test_remove(root);
    return result;
}
