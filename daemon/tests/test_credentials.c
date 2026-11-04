#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/credentials.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d\n", __LINE__);                                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static int exchange(int scenario) {
    int sockets[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(sockets[0]);
        unsigned char request[64];
        while (read(sockets[1], request, sizeof(request)) > 0) {
        }
        if (scenario == 2) {
            sleep(2);
            _exit(0);
        }
        unsigned char response[] = {0, 0, 0, 0, 7, 'f', 'i', 'x', 't', 'u', 'r', 'e'};
        if (scenario == 1)
            response[1] = 1; /* oversized length */
        if (scenario == 3)
            response[0] = 2; /* error with forbidden body */
        ssize_t ignored = write(sockets[1], response, sizeof(response));
        (void)ignored;
        close(sockets[1]);
        _exit(0);
    }
    close(sockets[1]);
    gstd_credential secret = {0};
    int status = gstd_credential_exchange(sockets[0], (const unsigned char *)"{}", 2, 100, &secret);
    if (!scenario) {
        CHECK(status == 0 && secret.length == 7 && !memcmp(secret.bytes, "fixture", 7));
    } else {
        CHECK(status < 0 && !secret.bytes && !secret.length);
    }
    gstd_credential_clear(&secret);
    CHECK(!secret.bytes && !secret.length);
    close(sockets[0]);
    kill(child, SIGKILL);
    CHECK(waitpid(child, NULL, 0) == child);
    return 0;
}
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < 4; i++)
        CHECK(!exchange(i));
    gstd_credential secret = {0};
    CHECK(gstd_credential_exchange(-1, (const unsigned char *)"{}", 2, 100, &secret) < 0);
    int fds[2];
    CHECK(!pipe(fds));
    CHECK(gstd_credential_exchange(fds[0], (const unsigned char *)"{}", 2, 100, &secret) < 0);
    close(fds[0]);
    close(fds[1]);
    CHECK(gstd_credential_run(-1, (const unsigned char *)"{}", 2, 100, &secret) < 0);
#ifdef __linux__
    CHECK(argc == 2);
    int executable = open(argv[1], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    CHECK(executable >= 0);
    int sentinel = fcntl(executable, F_DUPFD, 100);
    CHECK(sentinel >= 100);
    CHECK(!setenv("GITSTORE_FIXTURE_SENTINEL", "synthetic", 1));
    CHECK(!setenv("DOCKER_CONFIG", "/nonexistent-synthetic", 1));
    CHECK(!gstd_credential_run(executable, (const unsigned char *)"{}!", 3, 5000, &secret));
    CHECK(secret.length == 7 && !memcmp(secret.bytes, "fixture", 7));
    gstd_credential_clear(&secret);
    CHECK(fcntl(sentinel, F_GETFD) >= 0);
    close(sentinel);
    close(executable);
#else
    (void)argc;
    (void)argv;
#endif
    return 0;
}
