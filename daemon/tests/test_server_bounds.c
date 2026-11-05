#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/protocol.h"
#include "support.h"
#include <errno.h>
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
            fprintf(stderr, "server bounds line %d: %s\n", __LINE__, #x);                          \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static double seconds(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) ? -1 : now.tv_sec + now.tv_nsec / 1e9;
}
static int connect_peer(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (fd < 0 || strlen(path) >= sizeof(address.sun_path)) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    memcpy(address.sun_path, path, strlen(path) + 1);
    struct timeval timeout = {3, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
        connect(fd, (struct sockaddr *)&address, sizeof(address))) {
        close(fd);
        return -1;
    }
    return fd;
}
static int transfer(int fd, void *data, size_t length, int output) {
    unsigned char *bytes = data;
    while (length) {
        ssize_t n = output ? send(fd, bytes, length, 0) : recv(fd, bytes, length, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        bytes += n;
        length -= (size_t)n;
    }
    return 0;
}
static int request(int fd, unsigned id, int negotiate) {
    char data[512];
    int n = snprintf(data, sizeof(data),
                     "{\"version\":{\"major\":1,\"minor\":0},\"requestId\":\"r-%u\",\"method\":\"%"
                     "s\",\"params\":%s}",
                     id, negotiate ? "system.negotiate" : "system.capabilities",
                     negotiate
                         ? "{\"supported\":[{\"major\":1,\"minor\":0}],\"requiredCapabilities\":[]}"
                         : "{}");
    unsigned char prefix[] = {(unsigned)n >> 24, (unsigned)n >> 16, (unsigned)n >> 8, (unsigned)n};
    return n < 1 || (size_t)n >= sizeof(data) || transfer(fd, prefix, 4, 1) ||
                   transfer(fd, data, (size_t)n, 1)
               ? -1
               : 0;
}
static int response(int fd, unsigned id) {
    unsigned char prefix[4], bytes[8192];
    if (transfer(fd, prefix, 4, 0))
        return -1;
    size_t length = ((size_t)prefix[0] << 24) | ((size_t)prefix[1] << 16) |
                    ((size_t)prefix[2] << 8) | prefix[3];
    if (!length || length > sizeof(bytes) || transfer(fd, bytes, length, 0))
        return -1;
    json_t *value = gstd_json_parse(bytes, length);
    char expected[32];
    snprintf(expected, sizeof(expected), "r-%u", id);
    const char *found = json_string_value(json_object_get(value, "requestId"));
    int valid = found && !strcmp(found, expected) &&
                json_is_object(json_object_get(value, "result")) &&
                !json_object_get(value, "error");
    json_decref(value);
    return valid ? 0 : -1;
}
int main(int argc, char **argv) {
    int partial_only = argc == 3 && !strcmp(argv[2], "--partial-only");
    if (argc != 2 && !partial_only)
        return 1;
    gstd_json_initialize();
    signal(SIGPIPE, SIG_IGN);
    int result = 1, peers[33], status;
    for (unsigned i = 0; i < 33; i++)
        peers[i] = -1;
    char *root = gstd_test_dir(), path[512];
    pid_t child = -1;
    CHECK(root);
    snprintf(path, sizeof(path), "%s/gitstored.sock", root);
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        execl(argv[1], argv[1], "--runtime-dir", root, (char *)NULL);
        _exit(127);
    }
    double deadline = seconds() + 5;
    while ((peers[0] = connect_peer(path)) < 0) {
        CHECK(seconds() < deadline && waitpid(child, &status, WNOHANG) == 0);
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    for (unsigned i = 0; i < 32; i++) {
        if (i)
            peers[i] = connect_peer(path);
        CHECK(peers[i] >= 0 && !request(peers[i], i, 1) && !response(peers[i], i));
    }
    peers[32] = connect_peer(path);
    CHECK(peers[32] >= 0);
    char byte;
    CHECK(recv(peers[32], &byte, 1, 0) == 0);
    close(peers[32]);
    peers[32] = -1;
    /* Coalesced frames remain ordered; admission is serial and never accumulates
     * more than one unresolved request or response per connection. */
    for (unsigned i = 100; i < 117; i++)
        CHECK(!request(peers[0], i, 0));
    for (unsigned i = 100; i < 117; i++)
        CHECK(!response(peers[0], i));
    unsigned char oversized[] = {0, 16, 0, 1};
    CHECK(!transfer(peers[1], oversized, 4, 1));
    CHECK(recv(peers[1], &byte, 1, 0) == 0);
    close(peers[1]);
    peers[1] = -1;
    unsigned char partial[] = {0, 1};
    double partial_started = seconds();
    for (unsigned i = 2; i < 32; i++)
        CHECK(!transfer(peers[i], partial, 2, 1));
    while (seconds() - partial_started < 10.5) {
        CHECK(!request(peers[0], 200, 0) && !response(peers[0], 200));
        struct timespec delay = {0, 100000000};
        nanosleep(&delay, NULL);
    }
    for (unsigned i = 2; i < 32; i++) {
        CHECK(recv(peers[i], &byte, 1, 0) == 0);
        close(peers[i]);
        peers[i] = -1;
    }
    CHECK(!request(peers[0], 201, 0) && !response(peers[0], 201));
    /* Exercise the actual five-minute production timer, not a shortened test
     * constant or fake clock. This intentionally runs once per full suite. */
    if (!partial_only) {
        double idle_started = seconds();
        struct timeval idle_timeout = {305, 0};
        CHECK(!setsockopt(peers[0], SOL_SOCKET, SO_RCVTIMEO, &idle_timeout, sizeof(idle_timeout)));
        CHECK(recv(peers[0], &byte, 1, 0) == 0);
        CHECK(seconds() - idle_started >= 299 && seconds() - idle_started < 305);
    }
    close(peers[0]);
    peers[0] = -1;
    peers[0] = connect_peer(path);
    CHECK(peers[0] >= 0 && !request(peers[0], 300, 1) && !response(peers[0], 300));
    CHECK(waitpid(child, &status, WNOHANG) == 0 && !kill(child, SIGTERM));
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    child = -1;
    result = 0;
done:
    for (unsigned i = 0; i < 33; i++)
        if (peers[i] >= 0)
            close(peers[i]);
    if (child > 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    if (root)
        gstd_test_remove(root);
    return result;
}
