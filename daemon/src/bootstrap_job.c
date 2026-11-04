#define _GNU_SOURCE 1
#include "bootstrap_job.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include <string.h>
#if defined(__linux__) && defined(GITSTORED_BOOTSTRAP)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int one_thread(void) {
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
static int limit(int resource, rlim_t bound) {
    struct rlimit current;
    if (getrlimit(resource, &current))
        return -1;
    if (current.rlim_cur > bound)
        current.rlim_cur = bound;
    if (current.rlim_max > bound)
        current.rlim_max = bound;
    return setrlimit(resource, &current);
}
static int material(const gstd_bootstrap_job_input *input, gstd_credential *output) {
    json_t *message = json_pack("{s:{s:s,s:s,s:s,s:s},s:O}", "identity", "installation",
                                input->preparation.store_id, "issuer", "gitstore-local/core/1",
                                "account", input->preparation.deployment_id, "purpose",
                                "bootstrap-material", "source", input->source);
    char *encoded = message ? gstd_json_dump(message) : NULL;
    int result = encoded ? gstd_credential_run(input->broker_fd, (unsigned char *)encoded,
                                               strlen(encoded), 5000, output)
                         : -1;
    free(encoded);
    json_decref(message);
    return result;
}
static void run(gstd_bootstrap_job_input input, int writer, pid_t parent) {
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent || prctl(PR_SET_DUMPABLE, 0))
        _exit(126);
    gstd_network_namespace network = GSTD_NETWORK_NAMESPACE_INITIAL;
    if (input.network)
        network = *input.network;
    int original[] = {input.preparation.scope_fd, input.broker_fd, writer, network.user_fd,
                      network.network_fd},
        copies[5];
    for (unsigned i = 0; i < 5; i++) {
        copies[i] = original[i] < 0 ? -1 : fcntl(original[i], F_DUPFD_CLOEXEC, 32);
        if (original[i] >= 0 && copies[i] < 32)
            _exit(126);
    }
    for (unsigned i = 0; i < 5; i++) {
        if (copies[i] < 0)
            close((int)i + 3);
        else if (dup2(copies[i], (int)i + 3) != (int)i + 3 ||
                 fcntl((int)i + 3, F_SETFD, FD_CLOEXEC))
            _exit(126);
    }
    if (syscall(SYS_close_range, 8u, ~0u, 0u))
        _exit(126);
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0)
        _exit(126);
    for (int i = 0; i < 3; i++)
        if (dup2(null, i) != i)
            _exit(126);
    if (null >= 3)
        close(null);
    if (limit(RLIMIT_CORE, 0) || limit(RLIMIT_FSIZE, 67108864) || limit(RLIMIT_NOFILE, 128))
        _exit(126);
#ifndef GITSTORED_INSTRUMENTED
    if (limit(RLIMIT_AS, 1073741824))
        _exit(126);
#endif
    if (clearenv() || setenv("PATH", "/nonexistent", 1) || setenv("LANG", "C", 1))
        _exit(126);
    umask(077);
#ifdef GITSTORED_TESTING
    if (input.test_pause)
        raise(SIGSTOP);
#endif
    input.preparation.scope_fd = 3;
    input.broker_fd = 4;
    gstd_bootstrap_job_reply output = {.status = GSTD_BOOT_HTTP_REJECTED};
    if (input.kind == GSTD_BOOT_JOB_PREPARE) {
        output.status =
            gstd_bootstrap_provision(&input.preparation, 4, input.source, input.allow_initial);
    } else {
        gstd_credential secret = {0};
        if (!material(&input, &secret)) {
            if (input.kind == GSTD_BOOT_JOB_BIND)
                output.status =
                    gstd_bootstrap_controller_bind(&input.preparation, &secret, input.uid,
                                                   input.api_pid, input.api_start, input.api_boot);
            else {
                network.user_fd = 6;
                network.network_fd = 7;
                gstd_bootstrap_http_context context = {.control_owner = parent,
                                                       .network = &network};
                output.status = gstd_bootstrap_http(&context, input.action, &secret, output.uid);
            }
        }
        gstd_credential_clear(&secret);
    }
    ssize_t sent = write(5, &output, sizeof(output));
    close(5);
    _exit(sent == (ssize_t)sizeof(output) ? 0 : 1);
}
#endif
int gstd_bootstrap_job_launch(const gstd_bootstrap_job_input *input, unsigned timeout,
                              gstd_bootstrap_job *job) {
#if defined(__linux__) && defined(GITSTORED_BOOTSTRAP)
    struct sigaction sigchild;
    if (sigaction(SIGCHLD, NULL, &sigchild) || sigchild.sa_handler != SIG_DFL ||
        (sigchild.sa_flags & SA_NOCLDWAIT))
        return -1;
    if (!input || !job || job->worker || input->kind < GSTD_BOOT_JOB_PREPARE ||
        input->kind > GSTD_BOOT_JOB_HTTP || !timeout || timeout > 60000 ||
        input->preparation.control_owner != getpid() || input->preparation.scope_fd < 3 ||
        input->broker_fd < 3 || !geteuid() || !one_thread() ||
        gstd_asset_manifest_check(input->preparation.manifest) || !json_is_object(input->source) ||
        (input->kind == GSTD_BOOT_JOB_HTTP &&
         (!input->network || input->action < GSTD_BOOT_API_READY ||
          input->action > GSTD_BOOT_CONTROLLER_READY)) ||
        (input->kind == GSTD_BOOT_JOB_BIND && (!input->uid || !*input->uid || !input->api_boot ||
                                               input->api_pid <= 0 || !input->api_start)))
        return -1;
    if (input->test_pause > 1)
        return -1;
#ifndef GITSTORED_TESTING
    if (input->test_pause)
        return -1;
#endif
    int64_t now = now_ms();
    int fds[2];
    if (now < 0 || pipe2(fds, O_CLOEXEC | O_NONBLOCK))
        return -1;
    pid_t parent = getpid(), child = fork();
    if (!child)
        run(*input, fds[1], parent);
    close(fds[1]);
    if (child < 0) {
        close(fds[0]);
        return -1;
    }
    *job =
        (gstd_bootstrap_job){.worker = child, .descriptor = fds[0], .deadline_ms = now + timeout};
    return 0;
#else
    (void)input;
    (void)timeout;
    (void)job;
    return -1;
#endif
}
int gstd_bootstrap_job_poll(gstd_bootstrap_job *job, int cancel, gstd_bootstrap_job_reply *reply) {
    if (!job || job->worker <= 0 || !reply || cancel < -1)
        return -1;
#if defined(__linux__) && defined(GITSTORED_BOOTSTRAP)
    int64_t now = now_ms();
    struct pollfd cancellation = {.fd = cancel, .events = POLLIN};
    if (!job->interrupted &&
        (now < 0 || now >= job->deadline_ms ||
         (cancel >= 0 && poll(&cancellation, 1, 0) > 0 && cancellation.revents))) {
        if (kill(job->worker, SIGKILL) && errno != ESRCH)
            return -1;
        job->interrupted = 1;
    }
    if (!job->interrupted && job->received < sizeof(job->bytes)) {
        ssize_t count =
            read(job->descriptor, job->bytes + job->received, sizeof(job->bytes) - job->received);
        if (count > 0)
            job->received += (size_t)count;
        else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (kill(job->worker, SIGKILL) && errno != ESRCH)
                return -1;
            job->interrupted = 1;
        }
    }
    int status = 0;
    pid_t waited = waitpid(job->worker, &status, WNOHANG);
    if (!waited || (waited < 0 && errno == EINTR))
        return 0;
    *reply = (gstd_bootstrap_job_reply){.status = GSTD_BOOT_HTTP_UNCERTAIN};
    if (waited == job->worker && WIFEXITED(status) && !WEXITSTATUS(status) && !job->interrupted) {
        /* A final pipe read may lag the wait observation. Child's small report
         * is atomic and its descriptor is closed before successful exit. */
        if (job->received < sizeof(job->bytes)) {
            ssize_t n = read(job->descriptor, job->bytes + job->received,
                             sizeof(job->bytes) - job->received);
            if (n > 0)
                job->received += (size_t)n;
        }
        if (job->received == sizeof(job->bytes)) {
            gstd_bootstrap_job_reply candidate;
            memcpy(&candidate, job->bytes, sizeof(candidate));
            if (candidate.status >= -2 && candidate.status <= 1 &&
                memchr(candidate.uid, 0, sizeof(candidate.uid)))
                *reply = candidate;
        }
    }
    close(job->descriptor);
    memset(job, 0, sizeof(*job));
    job->descriptor = -1;
    return 1;
#else
    (void)cancel;
    return -1;
#endif
}
