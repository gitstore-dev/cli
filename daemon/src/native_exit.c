#define _GNU_SOURCE 1
#include "native_exit.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __linux__
#include <poll.h>
#include <sys/syscall.h>
#endif
#include <unistd.h>
int gstd_native_boot_id(char output[37]) {
    if (!output)
        return -1;
    output[0] = 0;
#ifdef __linux__
    int fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    char bytes[38];
    ssize_t count;
    do {
        count = read(fd, bytes, sizeof(bytes));
    } while (count < 0 && errno == EINTR);
    close(fd);
    if (count != 37 || bytes[36] != '\n')
        return -1;
    for (size_t i = 0; i < 36; i++) {
        int dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? bytes[i] != '-'
                 : !((bytes[i] >= '0' && bytes[i] <= '9') || (bytes[i] >= 'a' && bytes[i] <= 'f')))
            return -1;
    }
    memcpy(output, bytes, 36);
    output[36] = 0;
    return 0;
#else
    return -1;
#endif
}
int gstd_native_process_start(pid_t pid, uint64_t *output) {
    if (!output || pid <= 0)
        return -1;
    *output = 0;
#ifdef __linux__
    char path[64], bytes[4096];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    ssize_t length;
    do {
        length = read(fd, bytes, sizeof(bytes) - 1);
    } while (length < 0 && errno == EINTR);
    close(fd);
    if (length <= 0 || length >= (ssize_t)sizeof(bytes) - 1)
        return -1;
    bytes[length] = 0;
    char *last = strrchr(bytes, ')');
    if (!last || last[1] != ' ')
        return -1;
    char *position = last + 2;
    for (unsigned field = 3; field < 22; field++) {
        position = strchr(position, ' ');
        if (!position)
            return -1;
        position++;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(position, &end, 10);
    if (errno || !value || end == position || *position < '0' || *position > '9' || *end != ' ')
        return -1;
    *output = (uint64_t)value;
    return 0;
#else
    return -1;
#endif
}
int gstd_native_exit_poll(pid_t pid, uint64_t start, gstd_native_exit *output) {
    if (!output || pid <= 0 || !start)
        return -1;
    memset(output, 0, sizeof(*output));
#ifdef __linux__
    siginfo_t information = {0};
    if (waitid(P_PID, (id_t)pid, &information, WEXITED | WNOHANG | WNOWAIT))
        return errno == EINTR ? 0 : -2;
    if (!information.si_pid)
        return 0;
    uint64_t observed = 0;
    if (information.si_pid != pid || gstd_native_process_start(pid, &observed) || observed != start)
        return -2;
    int status;
    pid_t reaped = waitpid(pid, &status, WNOHANG);
    if (reaped != pid)
        return -2;
    if (WIFEXITED(status)) {
        output->exited = 1;
        output->code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        output->signalled = 1;
        output->code = WTERMSIG(status);
    } else
        return -2;
    return 1;
#else
    return -2;
#endif
}

int gstd_native_watch_open(pid_t pid, uint64_t start, const char *boot, int *output) {
    if (!output || *output != -1 || pid <= 0 || !start || !boot)
        return -1;
#ifdef __linux__
    char current[37], path[64];
    uint64_t actual;
    struct stat before, after;
    snprintf(path, sizeof(path), "/proc/%ld", (long)pid);
    if (gstd_native_boot_id(current) || strcmp(boot, current) || lstat(path, &before) ||
        !S_ISDIR(before.st_mode) || before.st_uid != geteuid() ||
        gstd_native_process_start(pid, &actual) || actual != start)
        return -1;
    int fd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (fd < 3 || lstat(path, &after) || !S_ISDIR(after.st_mode) || after.st_uid != geteuid() ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        gstd_native_process_start(pid, &actual) || actual != start ||
        gstd_native_boot_id(current) || strcmp(boot, current)) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    *output = fd;
    return 0;
#else
    return -1;
#endif
}
int gstd_native_watch_poll(int fd) {
#ifdef __linux__
    if (fd < 3)
        return -1;
    /* Private input must be the retained pidfd from watch_open, never caller fd.
     * Linux exposes pidfds as anon_inode:[pidfd]; reject ordinary readable fds. */
    char path[64], kind[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, kind, sizeof(kind));
    static const char expected[] = "anon_inode:[pidfd]";
    if (n != (ssize_t)(sizeof(expected) - 1) || memcmp(kind, expected, sizeof(expected) - 1))
        return -1;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int rc = poll(&p, 1, 0);
    if (rc < 0)
        return errno == EINTR ? 0 : -1;
    if (!rc)
        return 0;
    return (p.revents & POLLIN) && !(p.revents & (POLLERR | POLLNVAL)) ? 1 : -1;
#else
    (void)fd;
    return -1;
#endif
}
