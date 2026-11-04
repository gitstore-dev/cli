#define _GNU_SOURCE 1
#include "host_probe.h"
#include "gitstored/execution.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
const char *gstd_host_requirement_name(gstd_host_requirement requirement) {
    static const char *names[] = {"satisfied", "linux-kernel",      "user-session",
                                  "cgroup-v2", "cgroup-delegation", "user-bus",
                                  "tun",       "user-namespaces",   "pidfd",
                                  "openat2",   "runtime-library"};
    return (unsigned)requirement < sizeof(names) / sizeof(*names) ? names[requirement]
                                                                  : "runtime-library";
}
#if defined(__linux__) && defined(GITSTORED_LIBCRUN)
#include "native_abi.h"
#include <fcntl.h>
#include <linux/magic.h>
#include <linux/openat2.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
static int mapping(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    size_t size = strlen(text);
    int ok = write(fd, text, size) == (ssize_t)size;
    close(fd);
    return ok ? 0 : -1;
}
#endif
gstd_host_requirement gstd_host_probe(int state) {
#if defined(__linux__) && defined(GITSTORED_LIBCRUN)
    if (!gstd_execution_kernel_supported())
        return GSTD_HOST_KERNEL;
    uid_t uid = geteuid();
    gid_t gid = getegid();
    if (!uid)
        return GSTD_HOST_USER_SESSION;
    char path[256], bus[280], controllers[4096];
    struct stat st;
    struct statfs filesystem;
    snprintf(path, sizeof(path), "/run/user/%lu", (unsigned long)uid);
    if (lstat(path, &st) || !S_ISDIR(st.st_mode) || st.st_uid != uid ||
        (st.st_mode & 07777) != 0700)
        return GSTD_HOST_USER_SESSION;
    if (setenv("XDG_RUNTIME_DIR", path, 1))
        return GSTD_HOST_USER_SESSION;
    snprintf(bus, sizeof(bus), "unix:path=%s/bus", path);
    if (setenv("DBUS_SESSION_BUS_ADDRESS", bus, 1))
        return GSTD_HOST_USER_BUS;
    if (statfs("/sys/fs/cgroup", &filesystem) || filesystem.f_type != CGROUP2_SUPER_MAGIC)
        return GSTD_HOST_CGROUP;
    snprintf(path, sizeof(path),
             "/sys/fs/cgroup/user.slice/user-%lu.slice/user@%lu.service/cgroup.controllers",
             (unsigned long)uid, (unsigned long)uid);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return GSTD_HOST_DELEGATION;
    ssize_t size = read(fd, controllers + 1, sizeof(controllers) - 3);
    close(fd);
    if (size <= 0 || size >= (ssize_t)sizeof(controllers) - 3)
        return GSTD_HOST_DELEGATION;
    controllers[0] = ' ';
    controllers[size + 1] = ' ';
    controllers[size + 2] = 0;
    for (ssize_t i = 1; i <= size; i++)
        if (controllers[i] == '\n')
            controllers[i] = ' ';
    if (!strstr(controllers, " cpu ") || !strstr(controllers, " memory ") ||
        !strstr(controllers, " pids "))
        return GSTD_HOST_DELEGATION;
    if (!gstd_crun_host_ready())
        return GSTD_HOST_USER_BUS;
    fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return GSTD_HOST_TUN;
    int device = !fstat(fd, &st) && S_ISCHR(st.st_mode);
    close(fd);
    if (!device)
        return GSTD_HOST_TUN;
    fd = (int)syscall(SYS_pidfd_open, getpid(), 0);
    if (fd < 0)
        return GSTD_HOST_PIDFD;
    int supported = !syscall(SYS_pidfd_send_signal, fd, 0, NULL, 0);
    close(fd);
    if (!supported)
        return GSTD_HOST_PIDFD;
    struct open_how how = {.flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC,
                           .resolve = RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS};
    fd = (int)syscall(SYS_openat2, state, ".", &how, sizeof(how));
    if (fd < 0)
        return GSTD_HOST_OPENAT2;
    close(fd);
    /* No private material exists in this worker. Ordinary procfs dumpability is
     * needed for self-owned rootless mapping; CORE remains zero. */
    if (prctl(PR_SET_DUMPABLE, 1) || unshare(CLONE_NEWUSER))
        return GSTD_HOST_NAMESPACES;
    char map[80];
    snprintf(map, sizeof(map), "0 %lu 1\n", (unsigned long)uid);
    if (mapping("/proc/self/uid_map", map) || mapping("/proc/self/setgroups", "deny\n"))
        return GSTD_HOST_NAMESPACES;
    snprintf(map, sizeof(map), "0 %lu 1\n", (unsigned long)gid);
    if (mapping("/proc/self/gid_map", map) || unshare(CLONE_NEWNET | CLONE_NEWNS))
        return GSTD_HOST_NAMESPACES;
    return GSTD_HOST_OK;
#else
    (void)state;
    return GSTD_HOST_LIBRARY;
#endif
}
