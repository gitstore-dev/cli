#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t stopped;
#ifdef GSTD_REGISTERED_PAYLOAD
#define OUTPUT "/var/lib/gitstore/repositories"
#else
#define OUTPUT "/out"
#endif
static void stop(int signal_number) {
    (void)signal_number;
    stopped = 1;
}
int main(void) {
#ifdef GSTD_REGISTERED_PAYLOAD
    struct stat writer;
    if (fstat(3, &writer) || !S_ISREG(writer.st_mode) || writer.st_nlink != 1 || writer.st_size ||
        (fcntl(3, F_GETFD) & FD_CLOEXEC) || flock(3, LOCK_EX | LOCK_NB))
        return 15;
    struct rlimit bound;
    if (
#ifndef GITSTORED_INSTRUMENTED
        getrlimit(RLIMIT_AS, &bound) || bound.rlim_cur != (UINT64_C(1) << 40) ||
#endif
        getrlimit(RLIMIT_NOFILE, &bound) || bound.rlim_cur != 1024)
        return 10;
    int protected = open("/etc/gitstore/gitstore.toml", O_WRONLY);
    if (protected >= 0 || errno != EROFS)
        return 11;
    protected = open("/app/should-stay-readonly", O_CREAT | O_WRONLY, 0600);
    if (protected >= 0 || errno != EROFS)
        return 12;
    int temporary = open("/tmp/private-probe", O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (temporary < 0 || write(temporary, "private", 7) != 7)
        return 13;
    close(temporary);
    int null = open("/dev/null", O_WRONLY);
    if (null < 0 || write(null, "x", 1) != 1)
        return 14;
    close(null);
    FILE *resolver = fopen("/etc/resolv.conf", "r");
    if (resolver) {
        char bytes[128];
        size_t count = fread(bytes, 1, sizeof(bytes) - 1, resolver);
        bytes[count] = 0;
        fclose(resolver);
        if (strcmp(bytes, "nameserver 10.0.2.3\noptions timeout:2 attempts:2\n"))
            return 16;
        int writable = open("/etc/resolv.conf", O_WRONLY);
        if (writable >= 0 || errno != EROFS)
            return 17;
        int user = open("/proc/self/ns/user", O_RDONLY | O_CLOEXEC);
        int net = open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
        struct stat user_info, net_info;
        if (user < 0 || net < 0 || fstat(user, &user_info) || fstat(net, &net_info))
            return 18;
        close(user);
        close(net);
        int proof = open(OUTPUT "/namespaces", O_CREAT | O_EXCL | O_WRONLY, 0600);
        int length =
            snprintf(bytes, sizeof(bytes), "%llu %llu\n", (unsigned long long)user_info.st_ino,
                     (unsigned long long)net_info.st_ino);
        if (proof < 0 || length <= 0 || write(proof, bytes, (size_t)length) != length)
            return 19;
        close(proof);
    } else if (errno != ENOENT)
        return 20;
#endif
    DIR *fds = opendir("/proc/self/fd");
    if (!fds)
        return 2;
    struct dirent *entry;
    while ((entry = readdir(fds))) {
        char path[128], target[4096];
        int n = snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;
        ssize_t count = readlink(path, target, sizeof(target) - 1);
        if (count >= 0) {
            target[count] = 0;
            if (strstr(target, "sentinel-inherited") || strstr(target, ".gitstored-volume-lock") ||
                strstr(target, "/probe.db")) {
                closedir(fds);
                return 42;
            }
        }
    }
    closedir(fds);
#ifdef GSTD_REGISTERED_PAYLOAD
    const int require_security = 1;
#else
    const int require_security = getenv("GSTD_FIXTURE_REQUIRE_SECURITY") != NULL;
#endif
    if (require_security) {
        FILE *status = fopen("/proc/self/status", "r");
        char line[256];
        int nnp = 0, seccomp = 0, capabilities = 0;
        if (!status)
            return 7;
        while (fgets(line, sizeof(line), status)) {
            nnp |= !strcmp(line, "NoNewPrivs:\t1\n");
            seccomp |= !strcmp(line, "Seccomp:\t2\n");
            capabilities |= !strcmp(line, "CapEff:\t0000000000000000\n");
        }
        fclose(status);
        errno = 0;
        int clone_filter = getenv("GSTD_FIXTURE_SECURITY_FILTER") &&
                           !strcmp(getenv("GSTD_FIXTURE_SECURITY_FILTER"), "clone3");
#ifdef GSTD_REGISTERED_PAYLOAD
        clone_filter = 1;
#endif
        long filtered = clone_filter ? syscall(SYS_clone3, NULL, 0) : getpriority(PRIO_PROCESS, 0);
        if (!nnp || !seccomp || !capabilities || filtered != -1 ||
            errno != (clone_filter ? ENOSYS : EPERM))
            return 8;
        int marker = open(OUTPUT "/security", O_CREAT | O_EXCL | O_WRONLY, 0600);
        const char *proof = clone_filter
                                ? "nnp=1 seccomp=2 capabilities=0 explicit-filter=clone3-enosys\n"
                                : "nnp=1 seccomp=2 capabilities=0 explicit-filter=denied\n";
        if (marker < 0 || write(marker, proof, strlen(proof)) != (ssize_t)strlen(proof))
            return 9;
        close(marker);
    }
    struct sigaction action = {0};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL))
        return 3;
    int fd = open(OUTPUT "/lifecycle", O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd < 0)
        return 4;
    if (write(fd, "ready\n", 6) != 6)
        return 5;
    while (!stopped) {
        struct timespec delay = {0, 50000000};
        nanosleep(&delay, NULL);
    }
    if (write(fd, "terminated\n", 11) != 11)
        return 6;
    close(fd);
    return 0;
}
