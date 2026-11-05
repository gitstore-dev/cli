#define _GNU_SOURCE 1
#include "native_job.h"
#include "execution_plan.h"
#include "namespace_owner.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
typedef gstd_native_reply report;
_Static_assert(sizeof(report) == sizeof(((gstd_native_job *)0)->reply), "native reply layout");
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int owner_ready(void) {
    struct sigaction action;
    int subreaper = 0;
    if (sigaction(SIGCHLD, NULL, &action) || action.sa_handler != SIG_DFL ||
        (action.sa_flags & SA_NOCLDWAIT) || prctl(PR_GET_CHILD_SUBREAPER, &subreaper) || !subreaper)
        return 0;
    DIR *tasks = opendir("/proc/self/task");
    if (!tasks)
        return 0;
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(tasks)))
        if (entry->d_name[0] != '.')
            count++;
    closedir(tasks);
    return count == 1;
}
static int limit(int kind, rlim_t soft, int lower_hard) {
    struct rlimit value;
    if (getrlimit(kind, &value))
        return -1;
    if (value.rlim_cur > soft)
        value.rlim_cur = soft;
    if (lower_hard && value.rlim_max > soft)
        value.rlim_max = soft;
    return setrlimit(kind, &value);
}
static void worker(gstd_native_context context, gstd_native_action action, int writer, pid_t parent,
                   long maximum_fd) {
    /* Rootless uid/gid mapping requires ordinary same-user procfs access.
     * CORE remains zero. Credential/image workers retain their stricter separate
     * dumpability policy; this worker receives no registry/broker material. */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
        _exit(126);
    gstd_execution_inputs registered;
    gstd_network_namespace network = GSTD_NETWORK_NAMESPACE_INITIAL;
    size_t network_index = 0;
    int original[9 + GSTD_MOUNT_COUNT];
    size_t offset = context.writer_lease_fd >= 3 ? 1 : 0;
    if (offset)
        original[0] = context.writer_lease_fd;
    original[offset] = context.state_fd;
    original[offset + 1] = context.bundle_fd;
    original[offset + 2] = context.rootfs_fd;
    original[offset + 3] = writer;
    size_t count = offset + 4;
    if (context.registered_inputs) {
        registered = *context.registered_inputs;
        for (size_t i = 0; i < GSTD_MOUNT_COUNT; i++)
            original[count++] = registered.role_fds[i];
        if (registered.network) {
            network = *registered.network;
            network_index = count;
            original[count++] = network.user_fd;
            original[count++] = network.network_fd;
        }
    }
    size_t log_index = count;
    original[count++] = context.stdout_fd >= 3 ? context.stdout_fd : -1;
    original[count++] = context.stderr_fd >= 3 ? context.stderr_fd : -1;
    int copied[9 + GSTD_MOUNT_COUNT];
    for (size_t i = 0; i < count; i++)
        if (original[i] < 0)
            copied[i] = -1;
        else if ((copied[i] = fcntl(original[i], F_DUPFD_CLOEXEC, 32)) < 0)
            _exit(126);
    for (size_t i = 0; i < count; i++)
        if (copied[i] < 0)
            close(3 + (int)i);
        else if (dup2(copied[i], 3 + (int)i) < 0 || fcntl(3 + (int)i, F_SETFD, FD_CLOEXEC))
            _exit(126);
    context.state_fd = 3 + (int)offset;
    context.bundle_fd = 4 + (int)offset;
    context.rootfs_fd = 5 + (int)offset;
    int reply_fd = 6 + (int)offset;
    context.writer_lease_fd = offset ? 3 : -1;
    if (offset && fcntl(3, F_SETFD, 0))
        _exit(126);
    if (context.registered_inputs) {
        registered.rootfs_fd = context.rootfs_fd;
        for (size_t i = 0; i < GSTD_MOUNT_COUNT; i++)
            registered.role_fds[i] = original[offset + 4 + i] < 0 ? -1 : 7 + (int)offset + (int)i;
        context.registered_inputs = &registered;
        if (network_index) {
            network.user_fd = 3 + (int)network_index;
            network.network_fd = 4 + (int)network_index;
            network.life_fd = network.tap_fd = network.pid_fd = -1;
            registered.network = &network;
        }
    }
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0)
        _exit(126);
    for (int fd = 0; fd < 3; fd++)
        if (dup2(null, fd) < 0 || fcntl(fd, F_SETFD, 0))
            _exit(126);
    if (null >= 3)
        close(null);
    for (unsigned i = 0; i < 2; i++) {
        if (original[log_index + i] >= 3 &&
            (dup2(3 + (int)log_index + (int)i, 1 + (int)i) < 0 || fcntl(1 + (int)i, F_SETFD, 0)))
            _exit(126);
    }
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3u + (unsigned)count, UINT_MAX, 0u))
#endif
        for (int fd = 3 + (int)count; fd < maximum_fd; fd++)
            close(fd);
    char runtime[64], bus[96];
    snprintf(runtime, sizeof(runtime), "/run/user/%lu", (unsigned long)geteuid());
    snprintf(bus, sizeof(bus), "unix:path=%s/bus", runtime);
    if (clearenv() || setenv("PATH", "/nonexistent", 1) || setenv("LANG", "C", 1) ||
        setenv("XDG_RUNTIME_DIR", runtime, 1) || setenv("DBUS_SESSION_BUS_ADDRESS", bus, 1) ||
        limit(RLIMIT_CORE, 0, 1) ||
#ifndef GITSTORED_INSTRUMENTED
        limit(RLIMIT_AS, UINT64_C(1024) * 1024 * 1024, 0) ||
#endif
        limit(RLIMIT_FSIZE, UINT64_C(8) * 1024 * 1024 * 1024, 1) || limit(RLIMIT_NOFILE, 128, 0))
        _exit(126);
    report reply = {0};
    json_t *configuration = NULL;
    if (action == GSTD_NATIVE_CREATE && context.registered_inputs) {
        configuration = gstd_execution_build(context.registered_inputs);
        if (!configuration)
            _exit(126);
        context.configuration = configuration;
    }
    reply.status = (uint32_t)gstd_native_apply(&context, action, &reply.observed);
    json_decref(configuration);
    ssize_t sent;
    do {
        sent = write(reply_fd, &reply, sizeof(reply));
    } while (sent < 0 && errno == EINTR);
    _exit(sent == sizeof(reply) ? 0 : 126);
}
#endif
int gstd_native_launch(const gstd_native_context *context, gstd_native_action action,
                       unsigned timeout_ms, gstd_native_job *job) {
    if (!context || !job || job->worker || context->control_owner != getpid() || !timeout_ms ||
        timeout_ms > 30000 || action < GSTD_NATIVE_CREATE || action > GSTD_NATIVE_DELETE)
        return -1;
#ifdef __linux__
    if (action == GSTD_NATIVE_CREATE && context->registered_inputs) {
        const gstd_execution_inputs *input = context->registered_inputs;
        if (!input->component || !context->component ||
            strcmp(input->component, context->component) || input->rootfs_fd != context->rootfs_fd)
            return -1;
        json_t *registered = gstd_execution_build(input);
        int valid = registered && json_equal(registered, context->configuration);
        json_decref(registered);
        if (!valid)
            return -1;
    }
    int64_t start = now_ms();
    long maximum_fd = sysconf(_SC_OPEN_MAX);
    if (!gstd_native_backend_available() || !owner_ready() || prctl(PR_GET_DUMPABLE) != 1 ||
        start < 0 || maximum_fd < 0 || context->state_fd < 3 || context->bundle_fd < 3 ||
        context->rootfs_fd < 3)
        return -1;
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC))
        return -1;
    if (fcntl(descriptors[0], F_SETFL, O_NONBLOCK)) {
        close(descriptors[0]);
        close(descriptors[1]);
        return -1;
    }
    pid_t parent = getpid(), child = fork();
    if (!child)
        worker(*context, action, descriptors[1], parent, maximum_fd);
    close(descriptors[1]);
    if (child < 0) {
        close(descriptors[0]);
        return -1;
    }
    *job = (gstd_native_job){
        .worker = child, .descriptor = descriptors[0], .deadline_ms = start + timeout_ms};
    return 0;
#else
    return -1;
#endif
}
int gstd_native_poll(gstd_native_job *job, int cancellation_fd, gstd_native_status *status,
                     gstd_native_observation *observation) {
    if (!job || job->worker <= 0 || !status || !observation || cancellation_fd < -1)
        return -1;
#ifdef __linux__
    struct pollfd cancellation = {.fd = cancellation_fd, .events = POLLIN};
    int64_t now = now_ms();
    if (!job->interrupted &&
        (now < 0 || now >= job->deadline_ms ||
         (cancellation_fd >= 0 && poll(&cancellation, 1, 0) > 0 && cancellation.revents))) {
        job->interrupted = 1;
        kill(job->worker, SIGKILL);
    }
    while (job->received < sizeof(job->reply)) {
        ssize_t count =
            read(job->descriptor, job->reply + job->received, sizeof(job->reply) - job->received);
        if (count > 0)
            job->received += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else {
            if (count < 0 && errno != EAGAIN) {
                job->interrupted = 1;
                kill(job->worker, SIGKILL);
            }
            break;
        }
    }
    int exit_status = 0;
    pid_t reaped = waitpid(job->worker, &exit_status, WNOHANG);
    if (!reaped || (reaped < 0 && errno == EINTR))
        return 0;
    memset(observation, 0, sizeof(*observation));
    *status = GSTD_NATIVE_UNCERTAIN;
    observation->effect_possible = 1;
    if (reaped == job->worker && !job->interrupted && WIFEXITED(exit_status) &&
        !WEXITSTATUS(exit_status) && job->received == sizeof(report)) {
        report reply;
        memcpy(&reply, job->reply, sizeof(reply));
        if (reply.status <= GSTD_NATIVE_UNCERTAIN) {
            *status = (gstd_native_status)reply.status;
            *observation = reply.observed;
        }
    }
    close(job->descriptor);
    memset(job, 0, sizeof(*job));
    job->descriptor = -1;
    return 1;
#else
    (void)cancellation_fd;
    return -1;
#endif
}
