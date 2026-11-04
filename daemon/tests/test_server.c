#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/protocol.h"
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
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)
static pid_t launch(const char *binary, const char *root) {
    pid_t child = fork();
    if (!child) {
        execl(binary, binary, "--runtime-dir", root, (char *)NULL);
        _exit(127);
    }
    return child;
}
int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    char *temporary = gstd_test_dir(), *root = NULL;
    pid_t child = -1;
    int socket_fd = -1, result = 1, status;
    struct stat st;
    CHECK(temporary);
    root = realpath(temporary, NULL);
    CHECK(root);
    child = launch(argv[1], root);
    CHECK(child > 0);
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    CHECK(snprintf(path, sizeof(path), "%s/gitstored.sock", root) > 0);
    struct timespec started, now;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &started));
    for (;;) {
        CHECK(!clock_gettime(CLOCK_MONOTONIC, &now));
        if (now.tv_sec - started.tv_sec >= 5)
            break;
        CHECK(waitpid(child, &status, WNOHANG) == 0);
        if (!lstat(path, &st))
            break;
        struct timespec delay = {0, 20000000};
        nanosleep(&delay, NULL);
    }
    CHECK(!lstat(path, &st) && S_ISSOCK(st.st_mode) && (st.st_mode & 0777) == 0600 &&
          st.st_uid == getuid());
    pid_t duplicate = launch(argv[1], root);
    CHECK(duplicate > 0 && waitpid(duplicate, &status, 0) == duplicate && WIFEXITED(status) &&
          WEXITSTATUS(status) == 5);
    socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(socket_fd >= 0);
    struct timeval timeout = {2, 0};
    CHECK(!setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    CHECK(!connect(socket_fd, (struct sockaddr *)&address, sizeof(address)));
    const char payload[] = "{\"version\":{\"major\":1,\"minor\":0},\"requestId\":\"r1\",\"method\":"
                           "\"system.negotiate\",\"params\":{\"supported\":[{\"major\":1,\"minor\":"
                           "0}],\"requiredCapabilities\":[]}}";
    size_t n = strlen(payload);
    unsigned char prefix[4] = {(unsigned char)(n >> 24), (unsigned char)(n >> 16),
                               (unsigned char)(n >> 8), (unsigned char)n};
    for (size_t i = 0; i < 4; ++i)
        CHECK(write(socket_fd, prefix + i, 1) == 1);
    CHECK(write(socket_fd, payload, n) == (ssize_t)n);
    gstd_frame frame = {0};
    int complete = 0;
    while (!complete) {
        unsigned char bytes[8192];
        ssize_t got = read(socket_fd, bytes, sizeof(bytes));
        CHECK(got > 0);
        size_t consumed;
        complete = gstd_frame_feed(&frame, bytes, (size_t)got, &consumed);
        CHECK(complete >= 0 && consumed == (size_t)got);
    }
    json_t *response = gstd_json_parse(frame.payload, frame.used);
    gstd_frame_reset(&frame);
    CHECK(response);
    json_t *caps = json_object_get(json_object_get(response, "result"), "capabilities");
    int valid = json_array_size(caps) == 2 &&
                !strcmp(json_string_value(json_array_get(caps, 0)), "system.read");
    json_decref(response);
    CHECK(valid);
    CHECK(!kill(child, SIGTERM));
    CHECK(waitpid(child, &status, 0) == child);
    child = -1;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0 && lstat(path, &st) != 0 &&
          errno == ENOENT);
    CHECK(!chmod(root, 0755));
    pid_t unsafe = launch(argv[1], root);
    CHECK(unsafe > 0 && waitpid(unsafe, &status, 0) == unsafe && WIFEXITED(status) &&
          WEXITSTATUS(status) == 3);
    CHECK(!chmod(root, 0700));
    char link_path[256];
    CHECK(snprintf(link_path, sizeof(link_path), "%s/symlink", root) > 0);
    CHECK(!symlink(root, link_path));
    pid_t linked = launch(argv[1], link_path);
    CHECK(linked > 0 && waitpid(linked, &status, 0) == linked && WIFEXITED(status) &&
          WEXITSTATUS(status) == 3);
    int file = open(path, O_CREAT | O_WRONLY, 0600);
    CHECK(file >= 0);
    close(file);
    pid_t stale = launch(argv[1], root);
    CHECK(stale > 0 && waitpid(stale, &status, 0) == stale && WIFEXITED(status) &&
          WEXITSTATUS(status) == 3);
    CHECK(!lstat(path, &st) && S_ISREG(st.st_mode));
    result = 0;
done:
    if (socket_fd >= 0)
        close(socket_fd);
    if (child > 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }
    free(root);
    if (temporary)
        gstd_test_remove(temporary);
    return result;
}
