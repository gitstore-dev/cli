/* Synthetic installed-entry fixture, never linked into the daemon. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc != 2 || strcmp(argv[1], "__runtime-credential-broker-v1") || !getenv("PATH") ||
        strcmp(getenv("PATH"), "/nonexistent") || getenv("HOME") ||
        getenv("GITSTORE_FIXTURE_SENTINEL") || getenv("DOCKER_CONFIG") || getenv("HTTPS_PROXY"))
        return 1;
    for (int fd = 4; fd < 1024; fd++)
        if (fcntl(fd, F_GETFD) >= 0 || errno != EBADF)
            return 2;
    unsigned char request[7];
    size_t have = 0;
    while (have < sizeof(request)) {
        ssize_t count = read(3, request + have, sizeof(request) - have);
        if (count <= 0)
            return 3;
        have += (size_t)count;
    }
    if (memcmp(request, "\0\0\0\3{}!", 7))
        return 4;
    unsigned char end;
    if (read(3, &end, 1) != 0)
        return 5;
    const unsigned char result[] = {0, 0, 0, 0, 7, 'f', 'i', 'x', 't', 'u', 'r', 'e'};
    return write(3, result, sizeof(result)) == sizeof(result) ? 0 : 6;
}
