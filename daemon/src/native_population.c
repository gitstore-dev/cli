#define _GNU_SOURCE 1
#include "native_population.h"
#include "native_exit.h"
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <linux/openat2.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
static int kernel_group(int fd, struct stat *info) {
    struct statfs fs;
    return fd >= 3 && !fstat(fd, info) && S_ISDIR(info->st_mode) && !fstatfs(fd, &fs) &&
           fs.f_type == CGROUP2_SUPER_MAGIC;
}
static int membership(pid_t pid, char path[4096]) {
    char name[64], bytes[4096];
    snprintf(name, sizeof(name), "/proc/%ld/cgroup", (long)pid);
    int fd = open(name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    ssize_t n;
    do {
        n = read(fd, bytes, sizeof(bytes));
    } while (n < 0 && errno == EINTR);
    close(fd);
    if (n < 6 || n >= (ssize_t)sizeof(bytes) || memcmp(bytes, "0::/", 4) || bytes[n - 1] != '\n' ||
        memchr(bytes, 0, (size_t)n) || memchr(bytes, '\n', (size_t)n - 1))
        return -1;
    memcpy(path, bytes + 3, (size_t)n - 4);
    path[n - 4] = 0;
    return 0;
}
#endif
void gstd_native_population_close(gstd_native_population *group) {
    if (!group)
        return;
    if (group->directory_fd >= 3)
        close(group->directory_fd);
    if (group->events_fd >= 3)
        close(group->events_fd);
    *group = (gstd_native_population)GSTD_NATIVE_POPULATION_INITIAL;
}
int gstd_native_population_open(const char *id, pid_t pid, uint64_t start, const char *boot,
                                gstd_native_population *output) {
#ifdef __linux__
    if (!output || output->directory_fd != -1 || output->events_fd != -1 || !id || !*id ||
        strnlen(id, 65) > 64 || !start || !boot)
        return -1;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return -1;
    char current_boot[37], path[4096], after[4096], scope[96];
    uint64_t birth;
    if (gstd_native_boot_id(current_boot) || strcmp(boot, current_boot) ||
        gstd_native_process_start(pid, &birth) || birth != start || membership(pid, path))
        return -1;
    snprintf(scope, sizeof(scope), "/crun-%s.scope/", id);
    if (!strstr(path, scope))
        return -1;
    int root = open("/sys/fs/cgroup", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    if (!kernel_group(root, &info)) {
        if (root >= 0)
            close(root);
        return -1;
    }
    struct open_how how = {.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW,
                           .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS |
                                      RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV};
    int directory = (int)syscall(SYS_openat2, root, path + 1, &how, sizeof(how));
    close(root);
    int events = directory >= 3 ? openat(directory, "cgroup.events",
                                         O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)
                                : -1;
    struct stat event_info;
    struct statfs fs;
    int valid = events >= 3 && kernel_group(directory, &info) && info.st_nlink &&
                !fstat(events, &event_info) && S_ISREG(event_info.st_mode) &&
                event_info.st_dev == info.st_dev && !fstatfs(events, &fs) &&
                fs.f_type == CGROUP2_SUPER_MAGIC && !gstd_native_process_start(pid, &birth) &&
                birth == start && !membership(pid, after) && !strcmp(path, after);
    if (!valid) {
        if (events >= 0)
            close(events);
        if (directory >= 0)
            close(directory);
        return -1;
    }
    *output = (gstd_native_population){.directory_fd = directory,
                                       .events_fd = events,
                                       .device = (uint64_t)info.st_dev,
                                       .inode = (uint64_t)info.st_ino,
                                       .events_inode = (uint64_t)event_info.st_ino};
    return 0;
#else
    (void)id;
    (void)pid;
    (void)start;
    (void)boot;
    (void)output;
    return -1;
#endif
}
int gstd_native_population_empty(const gstd_native_population *group) {
#ifdef __linux__
    struct stat info, event;
    struct statfs fs;
    if (!group || !group->inode || group->directory_fd < 3 || fstat(group->directory_fd, &info) ||
        !S_ISDIR(info.st_mode) || (uint64_t)info.st_dev != group->device ||
        (uint64_t)info.st_ino != group->inode || group->events_fd < 3 ||
        fstat(group->events_fd, &event) || !S_ISREG(event.st_mode) || event.st_dev != info.st_dev ||
        (uint64_t)event.st_ino != group->events_inode)
        return -1;
    /* Kernel cgroup v2 removes a group only with no children/live processes:
     * https://www.kernel.org/doc/html/latest/admin-guide/cgroup-v2.html
     * This is a held kernel inode, not ENOENT from reopening a pathname. */
    if (fstatfs(group->directory_fd, &fs) || fs.f_type != CGROUP2_SUPER_MAGIC ||
        fstatfs(group->events_fd, &fs) || fs.f_type != CGROUP2_SUPER_MAGIC)
        return -1;
    if (!info.st_nlink)
        return 1;
    char bytes[4096];
    ssize_t n;
    do {
        n = pread(group->events_fd, bytes, sizeof(bytes) - 1, 0);
    } while (n < 0 && errno == EINTR);
    /* kernfs returns ENODEV when this held node is deactivated. Its cached
     * nlink can remain nonzero after removal; do not substitute pathname ENOENT.
     * https://github.com/torvalds/linux/blob/v6.18/fs/kernfs/file.c */
    if (n < 0 && errno == ENODEV)
        return 1;
    if (n <= 0 || n >= (ssize_t)sizeof(bytes) - 1 || memchr(bytes, 0, (size_t)n))
        return -1;
    bytes[n] = 0;
    int populated = -1;
    char *saved = NULL, *line = strtok_r(bytes, "\n", &saved);
    while (line) {
        if (!strncmp(line, "populated ", 10)) {
            if (populated != -1 || (strcmp(line, "populated 0") && strcmp(line, "populated 1")))
                return -1;
            populated = line[10] - '0';
        }
        line = strtok_r(NULL, "\n", &saved);
    }
    return populated < 0 ? -1 : !populated;
#else
    (void)group;
    return -1;
#endif
}
int gstd_native_population_matches(const gstd_native_population *group, const char *id, pid_t pid,
                                   uint64_t start, const char *boot) {
    if (!group)
        return -1;
    gstd_native_population current = GSTD_NATIVE_POPULATION_INITIAL;
    if (gstd_native_population_open(id, pid, start, boot, &current))
        return -1;
    int same = current.device == group->device && current.inode == group->inode &&
               current.events_inode == group->events_inode;
    gstd_native_population_close(&current);
    return same ? 0 : -1;
}
