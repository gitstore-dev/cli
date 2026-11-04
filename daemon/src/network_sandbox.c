#define _GNU_SOURCE 1
#include "network_sandbox.h"
#include "gitstored/execution.h"
#include "gitstored/protocol.h"
#include <stdlib.h>
#include <string.h>
static int excluded(const char *name) {
    static const char *const names[] = {"execve",
                                        "execveat",
                                        "clone",
                                        "clone3",
                                        "fork",
                                        "vfork",
                                        "socket",
                                        "socketpair",
                                        "ptrace",
                                        "process_vm_readv",
                                        "process_vm_writev",
                                        "pidfd_getfd",
                                        "open_by_handle_at",
                                        "name_to_handle_at",
                                        "io_uring_setup",
                                        "io_uring_enter",
                                        "io_uring_register",
                                        "mount",
                                        "umount",
                                        "umount2",
                                        "pivot_root",
                                        "chroot",
                                        "unshare",
                                        "setns",
                                        "kill",
                                        "tkill",
                                        "tgkill",
                                        "rt_sigqueueinfo",
                                        "rt_tgsigqueueinfo",
                                        "pidfd_open",
                                        "prlimit64",
                                        "sched_setparam",
                                        "sched_setscheduler",
                                        "sched_setattr",
                                        "sched_setaffinity",
                                        "setpriority",
                                        "move_pages",
                                        "migrate_pages",
                                        "kcmp"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i]))
            return 1;
    return 0;
}
json_t *gstd_network_worker_policy(void) {
    json_t *policy = gstd_execution_policy();
    json_t *rules = json_object_get(policy, "syscalls");
    for (size_t i = json_array_size(rules); i > 0; i--) {
        json_t *rule = json_array_get(rules, i - 1), *names = json_object_get(rule, "names");
        for (size_t j = json_array_size(names); j > 0; j--) {
            const char *name = json_string_value(json_array_get(names, j - 1));
            if (!name || (excluded(name) && json_array_remove(names, j - 1)))
                goto fail;
        }
        if (!json_array_size(names) && json_array_remove(rules, i - 1))
            goto fail;
    }
    /* libslirp needs only Internet sockets. In particular it cannot reconnect
     * to a host credential, runtime or session-bus Unix socket after setup. */
    for (int family = 2; family <= 10; family += 8) {
        json_t *rule =
            json_pack("{s:[s],s:s,s:[{s:i,s:i,s:s}]}", "names", "socket", "action",
                      "SCMP_ACT_ALLOW", "args", "index", 0, "value", family, "op", "SCMP_CMP_EQ");
        if (!rule || json_array_append_new(rules, rule))
            goto fail;
    }
    json_t *limit =
        json_pack("{s:[s],s:s,s:[{s:i,s:i,s:s}]}", "names", "prlimit64", "action", "SCMP_ACT_ALLOW",
                  "args", "index", 0, "value", 0, "op", "SCMP_CMP_EQ");
    if (!limit || json_array_append_new(rules, limit))
        goto fail;
    return policy;
fail:
    json_decref(policy);
    return NULL;
}
#if defined(__linux__) && defined(GITSTORED_SLIRP)
#include "native_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <sched.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static int exact_write(const char *path, const char *bytes) {
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    size_t length = strlen(bytes);
    ssize_t count;
    do {
        count = write(fd, bytes, length);
    } while (count < 0 && errno == EINTR);
    close(fd);
    return count == (ssize_t)length ? 0 : -1;
}
int gstd_network_sandbox(int scope) {
    char resolver[4097], mapping[64];
    int source = open("/etc/resolv.conf", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    struct stat info;
    if (source < 0)
        return -101;
    ssize_t length = fstat(source, &info) || !S_ISREG(info.st_mode)
                         ? -1
                         : read(source, resolver, sizeof(resolver));
    close(source);
    if (length <= 0 || length > 4096)
        return -102;
    uid_t uid = geteuid();
    gid_t gid = getegid();
    if (!uid || mkdirat(scope, "sandbox", 0700) || fchdir(scope))
        return -103;
    /* Select cwd before cloning mounts: a directory FD opened in the old
     * mount namespace still refers to that old mount after unshare. */
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWIPC) ||
        exact_write("/proc/self/setgroups", "deny\n"))
        return -104;
    snprintf(mapping, sizeof(mapping), "0 %lu 1\n", (unsigned long)uid);
    if (exact_write("/proc/self/uid_map", mapping))
        return -105;
    snprintf(mapping, sizeof(mapping), "0 %lu 1\n", (unsigned long)gid);
    if (exact_write("/proc/self/gid_map", mapping))
        return -201;
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL))
        return -202;
    if (mount("tmpfs", "sandbox", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC,
              "size=1048576,nr_inodes=32,mode=0700,uid=0,gid=0"))
        return -204;
    if (chdir("sandbox"))
        return -205;
    if (mkdir("etc", 0700))
        return -206;
    int fd = open("etc/resolv.conf", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0400);
    if (fd < 0)
        return -107;
    ssize_t written = write(fd, resolver, (size_t)length);
    close(fd);
    if (written != length ||
        mount(NULL, ".", NULL, MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) ||
        chroot(".") || chdir("/"))
        return -108;
    for (unsigned cap = 0;; cap++) {
        int present = prctl(PR_CAPBSET_READ, cap, 0, 0, 0);
        if (present < 0) {
            if (errno != EINVAL)
                return -109;
            break;
        }
        if (present && prctl(PR_CAPBSET_DROP, cap, 0, 0, 0))
            return -110;
    }
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct caps[2] = {{0}};
    if (syscall(SYS_capset, &header, caps) ||
        prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
        return -111;
    json_t *policy = gstd_network_worker_policy();
    json_t *body =
        policy ? json_pack("{s:s,s:{s:O}}", "ociVersion", "1.2.0", "linux", "seccomp", policy)
               : NULL;
    json_decref(policy);
    char *bytes = body ? gstd_json_dump(body) : NULL;
    json_decref(body);
    int result = bytes ? gstd_crun_worker_filter(bytes) : -1;
    free(bytes);
    return result;
}
#else
int gstd_network_sandbox(int scope) {
    (void)scope;
    return -1;
}
#endif
