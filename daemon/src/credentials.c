#define _GNU_SOURCE 1
#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "gitstored/credentials.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#define REQUEST_MAX 8192u
#define SECRET_MAX 16384u
static int64_t now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
void gstd_credential_clear(gstd_credential *secret) {
    if (!secret)
        return;
    if (secret->bytes) {
        volatile unsigned char *p = secret->bytes;
        for (size_t i = 0; i < secret->length; i++)
            p[i] = 0;
        free(secret->bytes);
    }
    secret->bytes = NULL;
    secret->length = 0;
}
static int transfer(int fd, unsigned char *buffer, size_t size, int writing, int64_t deadline) {
    while (size) {
        int64_t now = now_ms();
        if (now < 0 || now >= deadline)
            return -1;
        struct pollfd pollfd = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
        int ready = poll(&pollfd, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || (pollfd.revents & (POLLNVAL | POLLERR)))
            return -1;
        int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t count =
            writing ? send(fd, buffer, size, flags) : recv(fd, buffer, size, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count <= 0)
            return -1;
        buffer += count;
        size -= (size_t)count;
    }
    return 0;
}
static int peer(int fd) {
    struct sockaddr_storage address;
    socklen_t size = sizeof(address);
    int kind = 0;
    if (getsockname(fd, (struct sockaddr *)&address, &size) || address.ss_family != AF_UNIX)
        return -1;
    size = sizeof(kind);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &kind, &size) || kind != SOCK_STREAM)
        return -1;
#ifdef __linux__
    struct ucred owner;
    size = sizeof(owner);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &owner, &size) || owner.uid != geteuid())
        return -1;
#elif defined(__APPLE__)
    uid_t uid;
    gid_t gid;
    if (getpeereid(fd, &uid, &gid) || uid != geteuid())
        return -1;
#else
    return -1;
#endif
    return 0;
}
int gstd_credential_exchange(int fd, const unsigned char *request, size_t length,
                             unsigned timeout_ms, gstd_credential *out) {
    if (!out || out->bytes || out->length || !request || !length || length > REQUEST_MAX ||
        !timeout_ms || timeout_ms > 30000 || peer(fd))
        return -1;
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)))
        return -1;
#endif
    int64_t now = now_ms();
    if (now < 0)
        return -1;
    int64_t deadline = now + timeout_ms;
    unsigned char header[5] = {(unsigned char)(length >> 24), (unsigned char)(length >> 16),
                               (unsigned char)(length >> 8), (unsigned char)length, 0};
    if (transfer(fd, header, 4, 1, deadline) ||
        transfer(fd, (unsigned char *)request, length, 1, deadline) || shutdown(fd, SHUT_WR) ||
        transfer(fd, header, 5, 0, deadline))
        return -1;
    size_t bytes =
        (size_t)header[1] << 24 | (size_t)header[2] << 16 | (size_t)header[3] << 8 | header[4];
    if (header[0] > 2 || (header[0] && bytes) || (!header[0] && (!bytes || bytes > SECRET_MAX)))
        return -1;
    if (header[0])
        return header[0] == 1 ? 1 : -1;
    out->bytes = malloc(bytes);
    if (!out->bytes)
        return -1;
    out->length = bytes;
    if (transfer(fd, out->bytes, bytes, 0, deadline)) {
        gstd_credential_clear(out);
        return -1;
    }
    return 0;
}
int gstd_credential_run(int installed_fd, const unsigned char *request, size_t length,
                        unsigned timeout_ms, gstd_credential *out) {
#ifdef __linux__
    struct stat info;
    struct sigaction child_action;
    unsigned char magic[4];
    if (!out || out->bytes || out->length || !request || !length || length > REQUEST_MAX ||
        !timeout_ms || timeout_ms > 30000 || !geteuid() || fstat(installed_fd, &info) ||
        !S_ISREG(info.st_mode) || info.st_nlink != 1 || (info.st_mode & 0022) ||
        !(info.st_mode & 0111) || (info.st_uid != 0 && info.st_uid != geteuid()) ||
        pread(installed_fd, magic, 4, 0) != 4 || memcmp(magic, "\177ELF", 4) ||
        sigaction(SIGCHLD, NULL, &child_action) || child_action.sa_handler != SIG_DFL ||
        (child_action.sa_flags & SA_NOCLDWAIT))
        return -1;
    int64_t start = now_ms();
    if (start < 0)
        return -1;
    int executable = fcntl(installed_fd, F_DUPFD_CLOEXEC, 5), sockets[2] = {-1, -1};
    if (executable < 0)
        return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) {
        close(executable);
        return -1;
    }
    /* Build the environment before fork. Never copy registry/proxy/helper vars. */
    char bus[128];
    snprintf(bus, sizeof(bus), "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%ju/bus",
             (uintmax_t)geteuid());
    char *environment[] = {"PATH=/nonexistent", "LANG=C", bus, NULL};
    char *arguments[] = {"gs", "__runtime-credential-broker-v1", NULL};
    long maximum = sysconf(_SC_OPEN_MAX);
    if (maximum < 0 || maximum > 1048576)
        maximum = 1048576;
    pid_t child = fork();
    if (!child) {
        struct rlimit core = {0, 0};
        if (setrlimit(RLIMIT_CORE, &core) || dup2(sockets[1], 3) < 0 || dup2(executable, 4) < 0 ||
            fcntl(4, F_SETFD, FD_CLOEXEC) < 0)
            _exit(126);
        /* The copied socket at 3 must survive exec; all standard streams are null. */
        if (fcntl(3, F_SETFD, 0) < 0)
            _exit(126);
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null < 0)
            _exit(126);
        for (int fd = 0; fd < 3; fd++)
            if (dup2(null, fd) < 0)
                _exit(126);
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 5u, UINT_MAX, 0u))
#endif
            for (int fd = 5; fd < maximum; fd++)
                close(fd);
        fexecve(4, arguments, environment);
        _exit(126);
    }
    close(executable);
    close(sockets[1]);
    if (child < 0) {
        close(sockets[0]);
        return -1;
    }
    int64_t now = now_ms(), remaining = start + timeout_ms - now;
    int result = now < 0 || remaining <= 0 ? -1
                                           : gstd_credential_exchange(sockets[0], request, length,
                                                                      (unsigned)remaining, out);
    close(sockets[0]);
    int status = 0, reaped = 0, gone = 0;
    while (result >= 0) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            reaped = 1;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            if (errno == ECHILD)
                gone = 1;
            result = -1;
            break;
        }
        now = now_ms();
        if (now < 0 || now >= start + timeout_ms) {
            result = -1;
            break;
        }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    if (!reaped && !gone) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }
    if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) {
        gstd_credential_clear(out);
        return -1;
    }
    return result;
#else
    (void)installed_fd;
    (void)request;
    (void)length;
    (void)timeout_ms;
    (void)out;
    return -1;
#endif
}
