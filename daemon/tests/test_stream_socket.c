#define _POSIX_C_SOURCE 200809L
#include "gitstored/protocol.h"
#include "gitstored/state.h"
#include "logs.h"
#include "support.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "stream socket line %d: %s\n", __LINE__, #x);                          \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static int equal(const json_t *v, const char *key, const char *text) {
    const char *found = json_string_value(json_object_get(v, key));
    return found && !strcmp(found, text);
}
static int send_request(int fd, const char *id, const char *method, json_t *params) {
    json_t *request = json_pack("{s:{s:i,s:i},s:s,s:s,s:O}", "version", "major", 1, "minor", 0,
                                "requestId", id, "method", method, "params", params);
    char *bytes = gstd_json_dump(request);
    json_decref(request);
    if (!bytes)
        return -1;
    size_t n = strlen(bytes);
    unsigned char header[4] = {(unsigned char)(n >> 24), (unsigned char)(n >> 16),
                               (unsigned char)(n >> 8), (unsigned char)n};
    int failed = write(fd, header, 4) != 4 || write(fd, bytes, n) != (ssize_t)n;
    free(bytes);
    return failed ? -1 : 0;
}
static json_t *receive(int fd) {
    gstd_frame frame = {0};
    int status = 0;
    while (!status) {
        unsigned char bytes[8192];
        size_t remaining =
            frame.prefix_used < 4 ? 4 - frame.prefix_used : frame.expected - frame.used;
        if (remaining > sizeof(bytes))
            remaining = sizeof(bytes);
        ssize_t n = read(fd, bytes, remaining);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            gstd_frame_reset(&frame);
            return NULL;
        }
        size_t used;
        status = gstd_frame_feed(&frame, bytes, (size_t)n, &used);
    }
    json_t *v = status == 1 ? gstd_json_parse(frame.payload, frame.used) : NULL;
    gstd_frame_reset(&frame);
    return v;
}
int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    gstd_json_initialize();
    int result = 1, state = -1, directory = -1, fd = -1;
    pid_t child = -1;
    char *root = gstd_test_dir(), path[512], store[33], subscription[65];
    sqlite3 *db = NULL;
    gstd_logs *logs = NULL;
    json_t *value = NULL, *params = NULL, *payload = json_pack("{s:s}", "text", "socket fixture");
    CHECK(root && payload);
    snprintf(path, sizeof(path), "%s/state.db", root);
    CHECK(!gstd_state_open(path, &db));
    CHECK(sqlite3_exec(
              db,
              "INSERT INTO profiles VALUES('core','1','source','digest','linux-arm64');"
              "INSERT INTO deployments "
              "VALUES('observed','core','1','running',1,0,'pending','normal','released','2000-01-"
              "01T00:00:00Z',NULL);"
              "INSERT INTO components "
              "VALUES('observed','api',NULL,'digest','running','pending',1,'2000-01-01T00:00:00Z')",
              NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_stmt *q = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT store_id FROM query_meta", -1, &q, NULL) == SQLITE_OK &&
          sqlite3_step(q) == SQLITE_ROW);
    snprintf(store, sizeof(store), "%s", sqlite3_column_text(q, 0));
    sqlite3_finalize(q);
    CHECK(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    state = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(state >= 3);
    directory = gstd_logs_directory(state, store, 1);
    CHECK(directory >= 3 && gstd_logs_open_at(directory, NULL, 1, &logs) == GSTD_LOG_OK);
    for (unsigned i = 0; i < 300; i++)
        CHECK(gstd_logs_append(logs, "observed", "api", GSTD_STREAM_LOGS, payload,
                               (int64_t)time(NULL)) == GSTD_LOG_OK);
    gstd_logs_close(logs);
    logs = NULL;
    close(directory);
    directory = -1;
    close(state);
    state = -1;
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        execl(argv[1], argv[1], "--runtime-dir", root, "--state-dir", root, (char *)NULL);
        _exit(127);
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    CHECK(snprintf(address.sun_path, sizeof(address.sun_path), "%s/gitstored.sock", root) > 0);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    int connected = 0;
    for (unsigned i = 0; i < 500; i++) {
        if (!connect(fd, (struct sockaddr *)&address, sizeof(address))) {
            connected = 1;
            break;
        }
        int outcome;
        CHECK(waitpid(child, &outcome, WNOHANG) == 0);
        struct timespec pause = {0, 10000000};
        nanosleep(&pause, NULL);
    }
    CHECK(connected);
    struct timeval timeout = {5, 0};
    CHECK(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    params = json_pack("{s:[{s:i,s:i}],s:[]}", "supported", "major", 1, "minor", 0,
                       "requiredCapabilities");
    CHECK(params && !json_array_append_new(json_object_get(params, "requiredCapabilities"),
                                           json_string("stream.read")));
    CHECK(params && !send_request(fd, "hello", "system.negotiate", params));
    json_decref(params);
    params = NULL;
    value = receive(fd);
    CHECK(value && equal(value, "requestId", "hello") &&
          json_is_object(json_object_get(value, "result")));
    json_decref(value);
    value = NULL;
    params =
        json_pack("{s:s,s:s,s:s}", "deploymentId", "observed", "kind", "logs", "component", "api");
    CHECK(params && !send_request(fd, "subscribe", "stream.subscribe", params));
    value = receive(fd);
    CHECK(value && equal(value, "requestId", "subscribe"));
    const char *id =
        json_string_value(json_object_get(json_object_get(value, "result"), "subscriptionId"));
    CHECK(id && strlen(id) < sizeof(subscription));
    snprintf(subscription, sizeof(subscription), "%s", id);
    json_decref(value);
    value = NULL;
    for (unsigned i = 0; i < 300; i++) {
        value = receive(fd);
        CHECK(value && equal(value, "subscriptionId", subscription) && equal(value, "type", "log"));
        CHECK(!json_object_get(value, "requestId") &&
              json_integer_value(json_object_get(value, "sequence")) == i + 1);
        CHECK(equal(json_object_get(value, "payload"), "text", "socket fixture"));
        json_decref(value);
        value = NULL;
    }
    json_decref(params);
    params = json_pack("{s:s}", "subscriptionId", subscription);
    CHECK(params && !send_request(fd, "unsubscribe", "stream.unsubscribe", params));
    value = receive(fd);
    CHECK(value && equal(value, "requestId", "unsubscribe") &&
          json_is_true(json_object_get(json_object_get(value, "result"), "closed")));
    json_decref(value);
    value = NULL;
    CHECK(!send_request(fd, "closed", "stream.unsubscribe", params));
    value = receive(fd);
    CHECK(value && equal(value, "requestId", "closed") &&
          json_is_false(json_object_get(json_object_get(value, "result"), "closed")));
    json_decref(value);
    value = NULL;
    json_decref(params);
    params = json_pack("{s:s,s:s,s:s}", "deploymentId", "observed", "kind", "logs", "cursor",
                       "log:expired:1");
    CHECK(params && !send_request(fd, "expired", "stream.subscribe", params));
    value = receive(fd);
    CHECK(value && equal(value, "requestId", "expired") &&
          equal(json_object_get(value, "error"), "code", "CURSOR_EXPIRED"));
    result = 0;
done:
    if (fd >= 0)
        close(fd);
    if (child > 0) {
        (void)kill(child, SIGTERM);
        int outcome;
        (void)waitpid(child, &outcome, 0);
    }
    json_decref(value);
    json_decref(params);
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
