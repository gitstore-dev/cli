#define _GNU_SOURCE 1
#include "owned_read.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <fcntl.h>
#include <linux/capability.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define READ_MAGIC UINT32_C(0x47535244)
typedef struct {
    uint32_t magic, synchronize;
    uint64_t bytes;
} read_request;
static int64_t now_ms(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now)
               ? -1
               : (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static int mapping(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    int valid = write(fd, value, strlen(value)) == (ssize_t)strlen(value);
    close(fd);
    return valid ? 0 : -1;
}
static int trusted_executable(int fd, const struct stat *info) {
    if (!S_ISREG(info->st_mode) || (info->st_uid != 0 && info->st_uid != geteuid()) ||
        (info->st_mode & 06000))
        return 0;
    if (!(info->st_mode & 0022))
        return 1;
    /* libcrun re-executes its caller from a sealed memfd. Its permission bits
     * can include write access although the kernel forbids all content changes.
     * Accept only the pinned current executable with all four immutable seals. */
    int seals = fcntl(fd, F_GET_SEALS);
    const int required = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    return seals >= 0 && (seals & required) == required;
}
static int reader(void) {
    uid_t uid = geteuid();
    gid_t gid = getegid();
    struct ucred peer;
    socklen_t length = sizeof(peer);
    int kind = 0;
    socklen_t kind_length = sizeof(kind);
    struct stat file;
    read_request request;
    if (!uid || getuid() != uid || getgid() != gid ||
        getsockopt(3, SOL_SOCKET, SO_TYPE, &kind, &kind_length) || kind != SOCK_SEQPACKET ||
        getsockopt(3, SOL_SOCKET, SO_PEERCRED, &peer, &length) || peer.uid != uid ||
        peer.gid != gid || peer.pid != getppid() || prctl(PR_SET_PDEATHSIG, SIGKILL) ||
        peer.pid != getppid() || recv(3, &request, sizeof(request), MSG_TRUNC) != sizeof(request) ||
        request.magic != READ_MAGIC || request.synchronize > 1 ||
        request.bytes > UINT64_C(8) * 1024 * 1024 * 1024 || fstat(4, &file) ||
        !S_ISREG(file.st_mode) || file.st_uid != uid || file.st_gid != gid || file.st_size < 0 ||
        (uint64_t)file.st_size != request.bytes || !file.st_nlink || (file.st_mode & 06000) ||
        prctl(PR_GET_DUMPABLE) != 1)
        return 126;
    /* Exec discarded all inherited memory before this mapping window. Keep
     * numeric IDs unchanged so parent snapshot identity remains canonical. */
    if (unshare(CLONE_NEWUSER))
        return 126;
    char map[96];
    snprintf(map, sizeof(map), "%lu %lu 1\n", (unsigned long)uid, (unsigned long)uid);
    if (mapping("/proc/self/uid_map", map) || mapping("/proc/self/setgroups", "deny\n"))
        return 126;
    snprintf(map, sizeof(map), "%lu %lu 1\n", (unsigned long)gid, (unsigned long)gid);
    if (mapping("/proc/self/gid_map", map))
        return 126;
    for (unsigned cap = 0;; cap++) {
        int present = prctl(PR_CAPBSET_READ, cap, 0, 0, 0);
        if (present < 0) {
            if (errno != EINVAL)
                return 126;
            break;
        }
        if (present && cap != CAP_DAC_READ_SEARCH && prctl(PR_CAPBSET_DROP, cap, 0, 0, 0))
            return 126;
    }
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct caps[2] = {{0}};
    caps[0].effective = caps[0].permitted = UINT32_C(1) << CAP_DAC_READ_SEARCH;
    struct rlimit core = {0, 0}, files = {0, 0};
    if (syscall(SYS_capset, &header, caps) ||
        prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_DUMPABLE, 0) ||
        setrlimit(RLIMIT_CORE, &core) || setrlimit(RLIMIT_FSIZE, &files))
        return 126;
    int input = open("/proc/self/fd/4", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    struct stat opened;
    if (input < 0 || fstat(input, &opened) || opened.st_dev != file.st_dev ||
        opened.st_ino != file.st_ino || opened.st_size != file.st_size ||
        send(3, &request, sizeof(request), MSG_NOSIGNAL) != sizeof(request))
        return 126;
    unsigned char buffer[32768];
    uint64_t remaining = request.bytes;
    while (remaining) {
        size_t wanted = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        ssize_t got = read(input, buffer, wanted);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0 || send(3, buffer, (size_t)got, MSG_NOSIGNAL) != got)
            return 126;
        remaining -= (uint64_t)got;
    }
    unsigned char extra;
    int valid = read(input, &extra, 1) == 0 && (!request.synchronize || !fsync(input));
    close(input);
    return valid ? 0 : 126;
}
static int readable(int fd, int64_t deadline) {
    for (;;) {
        int64_t now = now_ms();
        if (now < 0 || now >= deadline)
            return -1;
        struct pollfd pending = {.fd = fd, .events = POLLIN};
        int result = poll(&pending, 1, (int)(deadline - now));
        if (result < 0 && errno == EINTR)
            continue;
        return result > 0 && (pending.revents & (POLLIN | POLLHUP)) ? 0 : -1;
    }
}
int gstd_owned_read(int source, uint64_t bytes, int synchronize,
                    int (*consume)(void *, const unsigned char *, size_t), void *context) {
    struct stat info, executable;
    if (source < 3 || !consume || bytes > UINT64_C(8) * 1024 * 1024 * 1024 ||
        (synchronize != 0 && synchronize != 1) || fstat(source, &info) || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || info.st_size < 0 || (uint64_t)info.st_size != bytes)
        return -1;
    int program = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    int channels[2];
    if (program < 3)
        return -1;
    if (fstat(program, &executable) || !trusted_executable(program, &executable) ||
        socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channels)) {
        close(program);
        return -1;
    }
    pid_t parent = getpid(), child = fork();
    if (!child) {
        int originals[] = {channels[1], source, program};
        int copied[3];
        for (unsigned i = 0; i < 3; i++)
            if ((copied[i] = fcntl(originals[i], F_DUPFD_CLOEXEC, 16)) < 0)
                _exit(126);
        for (unsigned i = 0; i < 3; i++)
            if (dup2(copied[i], 3 + (int)i) < 0 ||
                fcntl(3 + (int)i, F_SETFD, i == 2 ? FD_CLOEXEC : 0))
                _exit(126);
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null < 0 || prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
            _exit(126);
        for (int i = 0; i < 3; i++)
            if (dup2(null, i) < 0 || fcntl(i, F_SETFD, 0))
                _exit(126);
        if (null >= 6)
            close(null);
        if (syscall(SYS_close_range, 6u, ~0u, 0u))
            _exit(126);
        char *argv[] = {"gitstored", "--internal-owned-read", NULL};
        char *environment[] = {"PATH=/nonexistent", "LANG=C", "GITSTORED_INTERNAL_OWNED_READ=1",
                               NULL};
        syscall(SYS_execveat, 5, "", argv, environment, AT_EMPTY_PATH);
        _exit(126);
    }
    close(program);
    close(channels[1]);
    if (child < 0) {
        close(channels[0]);
        return -1;
    }
    int64_t now = now_ms(), deadline = now < 0 ? 0 : now + 60000;
    read_request request = {READ_MAGIC, (uint32_t)synchronize, bytes}, reply;
    int valid = now >= 0 &&
                send(channels[0], &request, sizeof(request), MSG_NOSIGNAL) == sizeof(request) &&
                !readable(channels[0], deadline) &&
                recv(channels[0], &reply, sizeof(reply), MSG_TRUNC) == sizeof(reply) &&
                !memcmp(&request, &reply, sizeof(request));
    uint64_t remaining = bytes;
    unsigned char buffer[32768];
    while (valid && remaining) {
        if (readable(channels[0], deadline))
            break;
        ssize_t got = recv(channels[0], buffer, sizeof(buffer), MSG_TRUNC);
        valid = got > 0 && (size_t)got <= sizeof(buffer) && (uint64_t)got <= remaining &&
                !consume(context, buffer, (size_t)got);
        if (valid)
            remaining -= (uint64_t)got;
    }
    valid = valid && !remaining && !readable(channels[0], deadline) &&
            recv(channels[0], buffer, sizeof(buffer), MSG_TRUNC) == 0;
    close(channels[0]);
    int status = 0;
    if (!valid)
        (void)kill(child, SIGKILL); /* direct unreaped child, never a native workload */
    pid_t reaped;
    do {
        reaped = waitpid(child, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    return valid && reaped == child && WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -1;
}
int gstd_owned_read_entry(int argc, char **argv) {
    return argc == 2 && !strcmp(argv[1], "--internal-owned-read") ? reader() : -1;
}
#ifdef GITSTORED_TESTING
/* Test/probe executables link the same private self-exec implementation. Their
 * main functions stay independent of daemon command-line parsing. */
__attribute__((constructor)) static void test_reader_entry(void) {
    const char *mode = getenv("GITSTORED_INTERNAL_OWNED_READ");
    if (mode && !strcmp(mode, "1"))
        _exit(reader());
}
#endif
#else
int gstd_owned_read(int fd, uint64_t bytes, int synchronize,
                    int (*consume)(void *, const unsigned char *, size_t), void *context) {
    (void)fd;
    (void)bytes;
    (void)synchronize;
    (void)consume;
    (void)context;
    return -1;
}
int gstd_owned_read_entry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return -1;
}
#endif
