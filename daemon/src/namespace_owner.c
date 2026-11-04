#define _GNU_SOURCE 1
#include "namespace_owner.h"
#include "native_exit.h"
#include <string.h>
#if defined(__linux__) && defined(GITSTORED_SLIRP)
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/if_tun.h>
#include <linux/magic.h>
#include <linux/nsfs.h>
#include <net/if.h>
#include <net/route.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define NS_MAGIC UINT32_C(0x47534e31)
typedef struct {
    uint32_t magic;
    pid_t pid;
    uint64_t start;
} note;
static int64_t now_ms(void) {
    struct timespec value;
    return clock_gettime(CLOCK_MONOTONIC, &value)
               ? -1
               : (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}
static int worker_ready(void) {
    for (int fd = 0; fd < 3; fd++)
        if (fcntl(fd, F_GETFD) < 0)
            return 0;
    struct sigaction action;
    struct rlimit bound;
    if (!geteuid() || prctl(PR_GET_DUMPABLE) != 1 || sigaction(SIGCHLD, NULL, &action) ||
        action.sa_handler != SIG_DFL || (action.sa_flags & SA_NOCLDWAIT) ||
        getrlimit(RLIMIT_CORE, &bound) || bound.rlim_cur)
        return 0;
#ifndef GITSTORED_INSTRUMENTED
    if (getrlimit(RLIMIT_AS, &bound) || bound.rlim_cur == RLIM_INFINITY ||
        bound.rlim_cur > UINT64_C(512) * 1024 * 1024)
        return 0;
#endif
    if (getrlimit(RLIMIT_FSIZE, &bound) || bound.rlim_cur == RLIM_INFINITY ||
        bound.rlim_cur > 67108864)
        return 0;
    DIR *directory = opendir("/proc/self/task");
    if (!directory)
        return 0;
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (entry->d_name[0] != '.')
            count++;
    closedir(directory);
    return count == 1;
}
static void write_mapping(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    size_t length = strlen(value);
    ssize_t count;
    if (fd < 0)
        _exit(126);
    do {
        count = write(fd, value, length);
    } while (count < 0 && errno == EINTR);
    close(fd);
    if (count != (ssize_t)length)
        _exit(126);
}
static struct sockaddr address(const char *value) {
    struct sockaddr_in native = {.sin_family = AF_INET};
    struct sockaddr result;
    if (inet_pton(AF_INET, value, &native.sin_addr) != 1)
        _exit(126);
    memcpy(&result, &native, sizeof(result));
    return result;
}
static void keeper(int channel, int lifeline, uid_t uid, gid_t gid, unsigned namespace_uid,
                   long maximum) {
    int first = fcntl(channel, F_DUPFD_CLOEXEC, 16), second = fcntl(lifeline, F_DUPFD_CLOEXEC, 16);
    if (first < 0 || second < 0 || dup2(first, 3) < 0 || dup2(second, 4) < 0 ||
        fcntl(3, F_SETFD, FD_CLOEXEC) || fcntl(4, F_SETFD, FD_CLOEXEC))
        _exit(126);
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0)
        _exit(126);
    for (int i = 0; i < 3; i++)
        if (dup2(null, i) < 0 || fcntl(i, F_SETFD, 0))
            _exit(126);
    if (null >= 3)
        close(null);
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 5u, UINT_MAX, 0u))
#endif
        for (int fd = 5; fd < maximum; fd++)
            close(fd);
    struct sigaction action = {.sa_handler = SIG_DFL};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL) || sigaction(SIGTERM, &action, NULL) ||
        sigaction(SIGINT, &action, NULL))
        _exit(126);
    alarm(10);
    note reply = {.magic = NS_MAGIC, .pid = getpid()};
    if (gstd_native_process_start(getpid(), &reply.start) || unshare(CLONE_NEWUSER))
        _exit(126);
    char map[80];
    snprintf(map, sizeof(map), "%u %lu 1\n", namespace_uid, (unsigned long)uid);
    write_mapping("/proc/self/uid_map", map);
    write_mapping("/proc/self/setgroups", "deny\n");
    snprintf(map, sizeof(map), "%u %lu 1\n", namespace_uid, (unsigned long)gid);
    write_mapping("/proc/self/gid_map", map);
    if (prctl(PR_SET_DUMPABLE, 1) || unshare(CLONE_NEWNET))
        _exit(126);
    int tap = open("/dev/net/tun", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    struct ifreq request = {.ifr_flags = IFF_TAP | IFF_NO_PI};
    memcpy(request.ifr_name, "gs0", 4);
    if (tap < 0 || ioctl(tap, TUNSETIFF, &request))
        _exit(126);
    int control = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (control < 0)
        _exit(126);
    request.ifr_addr = address("10.0.2.15");
    if (ioctl(control, SIOCSIFADDR, &request))
        _exit(126);
    request.ifr_netmask = address("255.255.255.0");
    if (ioctl(control, SIOCSIFNETMASK, &request))
        _exit(126);
    request.ifr_flags = IFF_UP;
    if (ioctl(control, SIOCSIFFLAGS, &request))
        _exit(126);
    memcpy(request.ifr_name, "lo", 3);
    request.ifr_flags = IFF_UP;
    if (ioctl(control, SIOCSIFFLAGS, &request))
        _exit(126);
    struct rtentry route = {.rt_dst = address("0.0.0.0"),
                            .rt_gateway = address("10.0.2.2"),
                            .rt_genmask = address("0.0.0.0"),
                            .rt_flags = RTF_UP | RTF_GATEWAY};
    if (ioctl(control, SIOCADDRT, &route))
        _exit(126);
    close(control);
    int fds[3] = {tap, open("/proc/self/ns/user", O_RDONLY | O_CLOEXEC),
                  open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC)};
    if (fds[1] < 0 || fds[2] < 0)
        _exit(126);
    char ancillary[CMSG_SPACE(sizeof(fds))] = {0};
    struct iovec vector = {&reply, sizeof(reply)};
    struct msghdr message = {.msg_iov = &vector,
                             .msg_iovlen = 1,
                             .msg_control = ancillary,
                             .msg_controllen = sizeof(ancillary)};
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(fds));
    memcpy(CMSG_DATA(header), fds, sizeof(fds));
    if (sendmsg(3, &message, MSG_NOSIGNAL) != (ssize_t)sizeof(reply))
        _exit(126);
    for (size_t i = 0; i < 3; i++)
        close(fds[i]);
    close(3);
    alarm(0);
    char byte;
    ssize_t count;
    do {
        count = read(4, &byte, 1);
    } while (count < 0 && errno == EINTR);
    close(4);
    _exit(count == 0 ? 0 : 126);
}
static int ns_fd(int fd, int type, struct stat *output) {
    struct statfs filesystem;
    return fd >= 3 && !fstat(fd, output) && !fstatfs(fd, &filesystem) &&
           filesystem.f_type == NSFS_MAGIC && ioctl(fd, NS_GET_NSTYPE) == type;
}
static int mapping(pid_t pid, const char *name, uint64_t expected, unsigned namespace_uid) {
    char path[80], bytes[128];
    snprintf(path, sizeof(path), "/proc/%ld/%s", (long)pid, name);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return 0;
    ssize_t count = read(fd, bytes, sizeof(bytes) - 1);
    close(fd);
    if (count <= 0 || count >= (ssize_t)sizeof(bytes) - 1)
        return 0;
    bytes[count] = 0;
    unsigned long long inside, outside, size;
    char extra;
    return sscanf(bytes, "%llu %llu %llu %c", &inside, &outside, &size, &extra) == 3 &&
           inside == namespace_uid && outside == expected && size == 1;
}
static int keeper_namespace(pid_t pid, const char *name, const struct stat *expected) {
    char path[80];
    snprintf(path, sizeof(path), "/proc/%ld/ns/%s", (long)pid, name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    struct stat observed;
    int valid = !fstat(fd, &observed) && observed.st_dev == expected->st_dev &&
                observed.st_ino == expected->st_ino;
    close(fd);
    return valid;
}
static int wait_owned(pid_t pid, int *status, unsigned timeout) {
    int64_t start = now_ms();
    if (start < 0)
        return -1;
    for (;;) {
        pid_t reaped = waitpid(pid, status, WNOHANG);
        if (reaped == pid)
            return 1;
        if (reaped < 0 && errno != EINTR)
            return -1;
        int64_t now = now_ms();
        if (now < 0 || now - start >= timeout)
            return 0;
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
}
#endif
int gstd_network_namespace_check(const gstd_network_namespace *owner) {
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    struct stat user, network, linked;
    char boot[37];
    uint64_t start = 0;
    uid_t uid;
    if (!owner || (owner->namespace_uid != 0 && owner->namespace_uid != 10000) ||
        owner->keeper_pid <= 0 || !owner->keeper_start || gstd_native_boot_id(boot) ||
        strcmp(owner->boot_id, boot) || !ns_fd(owner->user_fd, CLONE_NEWUSER, &user) ||
        !ns_fd(owner->network_fd, CLONE_NEWNET, &network) ||
        (uint64_t)user.st_dev != owner->user_device || (uint64_t)user.st_ino != owner->user_inode ||
        (uint64_t)network.st_dev != owner->network_device ||
        (uint64_t)network.st_ino != owner->network_inode ||
        ioctl(owner->user_fd, NS_GET_OWNER_UID, &uid) || uid != geteuid() ||
        gstd_native_process_start(owner->keeper_pid, &start) || start != owner->keeper_start ||
        !mapping(owner->keeper_pid, "uid_map", geteuid(), owner->namespace_uid) ||
        !mapping(owner->keeper_pid, "gid_map", getegid(), owner->namespace_uid) ||
        !keeper_namespace(owner->keeper_pid, "user", &user) ||
        !keeper_namespace(owner->keeper_pid, "net", &network))
        return -1;
    int parent = ioctl(owner->network_fd, NS_GET_USERNS);
    int valid = parent >= 0 && !fstat(parent, &linked) && linked.st_dev == user.st_dev &&
                linked.st_ino == user.st_ino;
    if (parent >= 0)
        close(parent);
    return valid ? 0 : -1;
#else
    (void)owner;
    return -1;
#endif
}
gstd_network_status gstd_network_namespace_open(gstd_network_namespace *owner) {
    return gstd_network_namespace_open_user(owner, 0);
}
gstd_network_status gstd_network_namespace_open_user(gstd_network_namespace *owner,
                                                     unsigned namespace_uid) {
    if ((namespace_uid != 0 && namespace_uid != 10000) || !owner || owner->keeper_pid ||
        owner->user_fd != -1 || owner->network_fd != -1 || owner->tap_fd != -1 ||
        owner->life_fd != -1 || owner->pid_fd != -1)
        return GSTD_NETWORK_INVALID;
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    owner->namespace_uid = namespace_uid;
    long maximum = sysconf(_SC_OPEN_MAX);
    char boot[37];
    if (maximum < 0 || !worker_ready() || gstd_native_boot_id(boot))
        return GSTD_NETWORK_HOST_UNMET;
    memcpy(owner->boot_id, boot, 37);
    int channels[2], life[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channels))
        return GSTD_NETWORK_FAILED;
    if (pipe2(life, O_CLOEXEC)) {
        close(channels[0]);
        close(channels[1]);
        return GSTD_NETWORK_FAILED;
    }
    pid_t child = fork();
    if (!child)
        keeper(channels[1], life[0], geteuid(), getegid(), namespace_uid, maximum);
    close(channels[1]);
    close(life[0]);
    if (child < 0) {
        close(channels[0]);
        close(life[1]);
        return GSTD_NETWORK_FAILED;
    }
    owner->keeper_pid = child;
    owner->life_fd = life[1];
    owner->pid_fd = (int)syscall(SYS_pidfd_open, child, 0);
    struct pollfd ready = {channels[0], POLLIN, 0};
    int available = poll(&ready, 1, 5000);
    int fds[3] = {-1, -1, -1};
    char ancillary[CMSG_SPACE(sizeof(fds))] = {0};
    note reply = {0};
    struct iovec vector = {&reply, sizeof(reply)};
    struct msghdr message = {.msg_iov = &vector,
                             .msg_iovlen = 1,
                             .msg_control = ancillary,
                             .msg_controllen = sizeof(ancillary)};
    ssize_t count =
        available > 0 ? recvmsg(channels[0], &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT) : -1;
    close(channels[0]);
    size_t received = 0, headers = 0;
    int descriptors = 1;
    if (count >= 0)
        for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header;
             header = CMSG_NXTHDR(&message, header)) {
            headers++;
            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
                header->cmsg_len < CMSG_LEN(0) || (header->cmsg_len - CMSG_LEN(0)) % sizeof(int)) {
                descriptors = 0;
                continue;
            }
            size_t number = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < number; i++) {
                int fd;
                memcpy(&fd, CMSG_DATA(header) + i * sizeof(int), sizeof(fd));
                if (received < 3)
                    fds[received++] = fd;
                else {
                    close(fd);
                    descriptors = 0;
                }
            }
        }
    int valid = count == (ssize_t)sizeof(reply) &&
                !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) && descriptors && headers == 1 &&
                received == 3 && reply.magic == NS_MAGIC && reply.pid == child && reply.start &&
                owner->pid_fd >= 3;
    if (valid) {
        owner->tap_fd = fds[0];
        owner->user_fd = fds[1];
        owner->network_fd = fds[2];
        owner->keeper_start = reply.start;
        struct stat user, network;
        valid = ns_fd(owner->user_fd, CLONE_NEWUSER, &user) &&
                ns_fd(owner->network_fd, CLONE_NEWNET, &network);
        if (valid) {
            owner->user_device = (uint64_t)user.st_dev;
            owner->user_inode = (uint64_t)user.st_ino;
            owner->network_device = (uint64_t)network.st_dev;
            owner->network_inode = (uint64_t)network.st_ino;
            valid = !gstd_network_namespace_check(owner);
        }
    } else
        for (size_t i = 0; i < 3; i++)
            if (fds[i] >= 0)
                close(fds[i]);
    if (valid)
        return GSTD_NETWORK_OK;
    /* This is our still-unreaped direct child, never a selected host PID. */
    if (owner->pid_fd >= 3)
        syscall(SYS_pidfd_send_signal, owner->pid_fd, SIGKILL, NULL, 0);
    else
        kill(child, SIGKILL);
    gstd_network_status cleanup = gstd_network_namespace_close(owner);
    return cleanup == GSTD_NETWORK_OK ? GSTD_NETWORK_HOST_UNMET : GSTD_NETWORK_FAILED;
#else
    return GSTD_NETWORK_UNAVAILABLE;
#endif
}
gstd_network_status gstd_network_namespace_close(gstd_network_namespace *owner) {
    if (!owner)
        return GSTD_NETWORK_OK;
#if defined(__linux__) && defined(GITSTORED_SLIRP)
    if (owner->keeper_pid > 0 && owner->pid_fd < 3 && owner->life_fd < 3)
        return GSTD_NETWORK_INVALID;
    int *fds[] = {&owner->tap_fd, &owner->user_fd, &owner->network_fd, &owner->life_fd};
    for (size_t i = 0; i < 4; i++) {
        if (*fds[i] >= 3)
            close(*fds[i]);
        *fds[i] = -1;
    }
    int status = 0,
        result = owner->keeper_pid > 0 ? wait_owned(owner->keeper_pid, &status, 500) : 1;
    if (!result && owner->pid_fd >= 3) {
        syscall(SYS_pidfd_send_signal, owner->pid_fd, SIGKILL, NULL, 0);
        result = wait_owned(owner->keeper_pid, &status, 500);
    }
    if (result != 1)
        return GSTD_NETWORK_FAILED;
    if (owner->pid_fd >= 3)
        close(owner->pid_fd);
    int clean = !owner->keeper_pid || (WIFEXITED(status) && !WEXITSTATUS(status));
    *owner = (gstd_network_namespace)GSTD_NETWORK_NAMESPACE_INITIAL;
    return clean ? GSTD_NETWORK_OK : GSTD_NETWORK_FAILED;
#else
    return owner->keeper_pid ? GSTD_NETWORK_UNAVAILABLE : GSTD_NETWORK_OK;
#endif
}
