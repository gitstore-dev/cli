#define _GNU_SOURCE 1
#include "libcrun/container.h"
#include "libcrun/status.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned callback_count;
static int worker_signal;
static pid_t workload_pid;
#ifdef GSTD_PROBE_CGROUPS
static int resource_value(const char *group, const char *name, const char *expected) {
    char path[4096], value[128];
    int length = snprintf(path, sizeof(path), "/sys/fs/cgroup%s/%s", group, name);
    if (length < 0 || (size_t)length >= sizeof(path))
        return -1;
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;
    int valid = fgets(value, sizeof(value), file) && !strcmp(value, expected);
    fclose(file);
    return valid ? 0 : -1;
}
#endif
static void output(int number, const char *message, int verbosity, void *argument) {
    (void)number;
    (void)verbosity;
    if (argument != stderr)
        _exit(96);
    callback_count++;
    fprintf(stderr, "libcrun fixture callback: %.240s\n", message ? message : "");
}
static libcrun_context_t context;
static char identifier[64];
static char bundle[4096], state_root[4096], configuration[4096];
static double monotonic(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value))
        return 0;
    return value.tv_sec + (double)value.tv_nsec / 1000000000;
}
struct reply {
    int result;
    unsigned callbacks;
};
static int worker(const char *stage) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC))
        return -1;
    pid_t child = fork();
    if (child < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (!child) {
        close(fds[0]);
        callback_count = 0;
        libcrun_set_verbosity(LIBCRUN_VERBOSITY_DEBUG);
        libcrun_error_t error = NULL;
        int result = -1;
        if (!strcmp(stage, "create")) {
            libcrun_container_t *container =
                libcrun_container_load_from_file(configuration, &error);
            if (container) {
                result = libcrun_container_create(&context, container, 0, &error);
                libcrun_container_free(container);
            }
        } else if (!strcmp(stage, "start") || !strcmp(stage, "start-crash")) {
            result = libcrun_container_start(&context, context.id, &error);
            if (result >= 0 && !strcmp(stage, "start-crash"))
                raise(SIGSEGV);
        } else if (!strcmp(stage, "stop"))
            result = libcrun_container_kill(&context, context.id, "SIGTERM", &error);
        else if (!strcmp(stage, "delete"))
            result = libcrun_container_delete(&context, NULL, context.id, false, &error);
        if (error) {
            fprintf(stderr, "libcrun fixture %s: %.240s\n", stage, error->msg ? error->msg : "");
            libcrun_error_release(&error);
        }
        if (!strcmp(stage, "create") && result >= 0 && callback_count == 0) {
            fputs("Required native callback not exercised\n", stderr);
            result = -1;
        }
        struct reply reply = {result, callback_count};
        ssize_t sent = write(fds[1], &reply, sizeof(reply));
        close(fds[1]);
        _exit(sent == (ssize_t)sizeof(reply) && result >= 0 ? 0 : 1);
    }
    close(fds[1]);
    struct pollfd descriptor = {fds[0], POLLIN, 0};
    struct reply reply = {-1, 0};
    int ready = poll(&descriptor, 1, 10000);
    ssize_t received = ready > 0 ? read(fds[0], &reply, sizeof(reply)) : -1;
    close(fds[0]);
    int status = 0;
    double deadline = monotonic() + 5;
    while (waitpid(child, &status, WNOHANG) == 0) {
        if (monotonic() >= deadline) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            return -1;
        }
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    worker_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    if (received != (ssize_t)sizeof(reply) || !WIFEXITED(status) || WEXITSTATUS(status) ||
        reply.result < 0)
        return -1;
    printf("{\"stage\":\"%s\",\"result\":%d,\"callbacks\":%u}\n", stage, reply.result,
           reply.callbacks);
    return 0;
}
static int running(int expected) {
    libcrun_error_t error = NULL;
    libcrun_container_status_t status = {0};
    const char *name = NULL;
    int active = 0;
    if (libcrun_read_container_status(&status, state_root, context.id, &error) < 0)
        goto fail;
    if (status.pid <= 0 || !status.process_start_time)
        goto fail;
    if (libcrun_get_container_state_string(context.id, &status, state_root, &name, &active,
                                           &error) < 0)
        goto fail;
    if (active)
        workload_pid = status.pid;
#ifdef GSTD_PROBE_CGROUPS
    if (active && (!status.systemd_cgroup || !status.cgroup_path ||
                   strncmp(status.cgroup_path, "/user.slice/", 12) ||
                   resource_value(status.cgroup_path, "memory.max", "134217728\n") ||
                   resource_value(status.cgroup_path, "pids.max", "64\n") ||
                   resource_value(status.cgroup_path, "cpu.max", "50000 100000\n")))
        goto fail;
#endif
    libcrun_free_container_status(&status);
    if (error)
        libcrun_error_release(&error);
    return (!!active) == expected ? 0 : -1;
fail:
    if (error) {
        fprintf(stderr, "libcrun fixture state: %.240s\n", error->msg ? error->msg : "");
        libcrun_error_release(&error);
    }
    libcrun_free_container_status(&status);
    return -1;
}
int main(int argc, char **argv) {
    if ((argc != 2 && argc != 3) || (argc == 3 && strcmp(argv[2], "start-crash")) ||
        geteuid() == 0) {
        fputs("Select an owned private non-root fixture directory\n", stderr);
        return 77;
    }
    struct stat metadata;
    if (lstat(argv[1], &metadata) || !S_ISDIR(metadata.st_mode) || metadata.st_uid != geteuid() ||
        (metadata.st_mode & 0777) != 0700)
        return 2;
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0))
        return 1;
    struct rlimit no_core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &no_core))
        return 1;
    unsigned char random[16];
    int entropy = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (entropy < 0)
        return 1;
    size_t have = 0;
    while (have < sizeof(random)) {
        ssize_t count = read(entropy, random + have, sizeof(random) - have);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            close(entropy);
            return 1;
        }
        have += (size_t)count;
    }
    close(entropy);
    memcpy(identifier, "gstd-probe-", 11);
    for (size_t i = 0; i < sizeof(random); i++)
        snprintf(identifier + 11 + 2 * i, 3, "%02x", random[i]);
    if (snprintf(bundle, sizeof(bundle), "%s/bundle", argv[1]) < 0 ||
        snprintf(state_root, sizeof(state_root), "%s/state", argv[1]) < 0 ||
        snprintf(configuration, sizeof(configuration), "%s/bundle/config.json", argv[1]) < 0)
        return 2;
    context = (libcrun_context_t){.state_root = state_root,
                                  .id = identifier,
                                  .bundle = bundle,
                                  .detach = true,
                                  .no_new_keyring = true,
#ifdef GSTD_PROBE_CGROUPS
                                  .systemd_cgroup = true,
#else
                                  .force_no_cgroup = true,
#endif
                                  .output_handler = output,
                                  .output_handler_arg = stderr};
    char sentinel[4096];
    int n = snprintf(sentinel, sizeof(sentinel), "%s/sentinel-inherited", argv[1]);
    if (n < 0 || (size_t)n >= sizeof(sentinel))
        return 2;
    int fd = open(sentinel, O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        return 1; /* deliberately lacks CLOEXEC */
    int crashed = argc == 3;
    int created = worker("create");
    int started = created ? -1 : worker(crashed ? "start-crash" : "start");
    if (created || (!crashed && started) || (crashed && (!started || worker_signal != SIGSEGV))) {
        close(fd);
        return 1;
    }
    char marker[4096];
    n = snprintf(marker, sizeof(marker), "%s/output/lifecycle", argv[1]);
    if (n < 0 || (size_t)n >= sizeof(marker)) {
        close(fd);
        return 2;
    }
    double deadline = monotonic() + 5;
    while (access(marker, F_OK)) {
        if (monotonic() >= deadline) {
            worker("stop");
            worker("delete");
            close(fd);
            return 1;
        }
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    if (running(1)) {
        close(fd);
        return 1;
    }
    pid_t crash = fork();
    if (crash < 0) {
        close(fd);
        return 1;
    }
    if (!crash) {
        raise(SIGSEGV);
        _exit(99);
    }
    int status;
    if (waitpid(crash, &status, 0) != crash || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGSEGV || running(1)) {
        close(fd);
        return 1;
    }
    if (worker("stop")) {
        close(fd);
        return 1;
    }
    deadline = monotonic() + 5;
    while (running(0)) {
        if (monotonic() >= deadline) {
            close(fd);
            return 1;
        }
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    deadline = monotonic() + 5;
    pid_t reaped;
    do {
        reaped = waitpid(workload_pid, &status, WNOHANG);
        if (reaped < 0) {
            close(fd);
            return 1;
        }
        if (!reaped) {
            struct timespec delay = {0, 10000000};
            nanosleep(&delay, NULL);
        }
    } while (!reaped && monotonic() < deadline);
    if (reaped != workload_pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        close(fd);
        return 1;
    }
    puts("PASS: subreaper collected actual payload exit code 0");
    if (worker("delete")) {
        close(fd);
        return 1;
    }
    close(fd);
#ifdef GSTD_PROBE_CGROUPS
    puts("PASS: delegated memory/pids/cpu limits, create/start/state/kill/delete, worker crash "
         "survival and inherited-FD marker");
#else
    puts("PASS: namespace-only create/start/state/kill/delete, worker exit/crash survival and "
         "inherited-FD marker");
#endif
    return 0;
}
