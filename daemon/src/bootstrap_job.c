#define _GNU_SOURCE 1
#include "bootstrap_job.h"
#include "gitstored/assets.h"
#include "gitstored/protocol.h"
#include "oidc_config.h"
#include "oidc_material.h"
#include "oidc_preparation.h"
#include "oidc_profile.h"
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
    int oidc = input->kind == GSTD_BOOT_JOB_OIDC_HTTP;
    json_t *message = json_pack(
        "{s:{s:s,s:s,s:s,s:s},s:O}", "identity", "installation", input->preparation.store_id,
        "issuer", oidc ? "gitstore-local/reference-oidc/1" : "gitstore-local/core/1", "account",
        input->preparation.deployment_id, "purpose",
        oidc ? "oidc-provider-material" : "bootstrap-material", "source", input->source);
    char *encoded = message ? gstd_json_dump(message) : NULL;
    int result = encoded ? gstd_credential_run(input->broker_fd, (unsigned char *)encoded,
                                               strlen(encoded), 5000, output)
                         : -1;
    free(encoded);
    json_decref(message);
    return result;
}
static gstd_oidc_http_status oidc_http(const gstd_bootstrap_job_input *input,
                                       const gstd_bootstrap_http_context *context,
                                       const gstd_credential *material) {
    /* No missing preparation or missing material may be repaired as a side
     * effect of an HTTP job. The parent committed the exact file receipt first. */
    struct stat prepared;
    if (fstatat(input->preparation.scope_fd, "prepared.json", &prepared, AT_SYMLINK_NOFOLLOW) ||
        !S_ISREG(prepared.st_mode) || prepared.st_uid != geteuid() ||
        (prepared.st_mode & 07777) != 0600 || prepared.st_nlink != 1 ||
        gstd_oidc_material_check(material))
        return GSTD_OIDC_REJECTED;
    int files[4] = {-1, -1, -1, -1};
    int valid = !gstd_oidc_prepare(&input->preparation, &input->oidc_ports, material, files);
    for (size_t i = 0; i < 4; i++)
        if (files[i] >= 3)
            close(files[i]);
    if (!valid)
        return GSTD_OIDC_REJECTED;
    json_t *family = gstd_json_parse(material->bytes, material->length);
    const json_t *client = json_object_get(family, "clientSecret");
    gstd_credential secret = {.bytes = (unsigned char *)json_string_value(client),
                              .length = json_string_length(client)};
    gstd_oidc_http_status result =
        gstd_oidc_http(context, input->oidc_action,
                       input->oidc_action == GSTD_OIDC_HTTP_READY ? NULL : &input->oidc_client,
                       input->oidc_action == GSTD_OIDC_HTTP_CREATE ? &secret : NULL);
    gstd_oidc_config_clear(family);
    json_decref(family);
    return result;
}
static void run(gstd_bootstrap_job_input input, int writer, pid_t parent) {
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent || prctl(PR_SET_DUMPABLE, 0))
        _exit(126);
    gstd_network_namespace network = GSTD_NETWORK_NAMESPACE_INITIAL;
    if (input.network)
        network = *input.network;
    /* Read-only health jobs carry no configuration or credential capability. */
    int health = input.kind == GSTD_BOOT_JOB_OIDC_HEALTH;
    int original[] = {health ? -1 : input.preparation.scope_fd, health ? -1 : input.broker_fd,
                      writer, network.user_fd,
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
    } else if (input.kind == GSTD_BOOT_JOB_OIDC_HEALTH) {
        network.user_fd = 6;
        network.network_fd = 7;
        gstd_bootstrap_http_context context = {.control_owner = parent, .network = &network};
        output.status = gstd_oidc_health(&context, input.oidc_health_role, &input.oidc_ports);
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
                output.status =
                    input.kind == GSTD_BOOT_JOB_OIDC_HTTP
                        ? oidc_http(&input, &context, &secret)
                        : gstd_bootstrap_http(&context, input.action, &secret, output.uid);
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
        input->kind > GSTD_BOOT_JOB_OIDC_HEALTH || !timeout || timeout > 60000 ||
        input->preparation.control_owner != getpid() || input->preparation.scope_fd < 3 ||
        (input->kind != GSTD_BOOT_JOB_OIDC_HEALTH && input->broker_fd < 3) || !geteuid() ||
        !one_thread() || gstd_asset_manifest_check(input->preparation.manifest) ||
        (input->kind != GSTD_BOOT_JOB_OIDC_HEALTH && !json_is_object(input->source)) ||
        (input->kind == GSTD_BOOT_JOB_HTTP &&
         (!input->network || input->action < GSTD_BOOT_API_READY ||
          input->action > GSTD_BOOT_CONTROLLER_READY)) ||
        (input->kind == GSTD_BOOT_JOB_OIDC_HTTP &&
         (!input->network || input->allow_initial ||
          gstd_oidc_core_manifest_check(input->preparation.manifest) ||
          !json_object_get(input->preparation.manifest, "oidcProfileDigest") ||
          gstd_oidc_ports_check(&input->oidc_ports) || input->oidc_action < GSTD_OIDC_HTTP_READY ||
          input->oidc_action > GSTD_OIDC_HTTP_INSPECT ||
          (input->oidc_action != GSTD_OIDC_HTTP_READY &&
           gstd_oidc_client_check(&input->oidc_client)))) ||
        (input->kind == GSTD_BOOT_JOB_OIDC_HEALTH &&
         (!input->network || input->allow_initial ||
          gstd_oidc_core_manifest_check(input->preparation.manifest) ||
          !json_object_get(input->preparation.manifest, "oidcProfileDigest") ||
          gstd_oidc_ports_check(&input->oidc_ports) ||
          input->oidc_health_role < GSTD_OIDC_HEALTH_HYDRA ||
          input->oidc_health_role > GSTD_OIDC_HEALTH_ISSUER)) ||
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
    *job = (gstd_bootstrap_job){.worker = child,
                                .descriptor = fds[0],
                                .deadline_ms = now + timeout,
                                .kind = input->kind,
                                .oidc_action = input->oidc_action};
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
    /* A fully reaped reply belongs to the original operation even if STOP is
     * now ready. In particular, do not discard a known creation ACK by killing
     * an already completed worker. A reply without observed exit is not enough. */
    int status = 0;
    pid_t waited = waitpid(job->worker, &status, WNOHANG);
    if (waited < 0 && errno == EINTR)
        return 0;
    if (!waited) {
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
            ssize_t count = read(job->descriptor, job->bytes + job->received,
                                 sizeof(job->bytes) - job->received);
            if (count > 0)
                job->received += (size_t)count;
            else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                if (kill(job->worker, SIGKILL) && errno != ESRCH)
                    return -1;
                job->interrupted = 1;
            }
        }
        waited = waitpid(job->worker, &status, WNOHANG);
        if (!waited || (waited < 0 && errno == EINTR))
            return 0;
    }
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
            int valid = candidate.status >= -2 && candidate.status <= 1 &&
                        memchr(candidate.uid, 0, sizeof(candidate.uid));
            if (job->kind == GSTD_BOOT_JOB_OIDC_HTTP) {
                static const char empty[129] = {0};
                int success = job->oidc_action == GSTD_OIDC_HTTP_READY    ? GSTD_OIDC_PROVIDER_READY
                              : job->oidc_action == GSTD_OIDC_HTTP_CREATE ? GSTD_OIDC_CLIENT_CREATED
                                                                          : GSTD_OIDC_PUBLIC_MATCH;
                valid = !memcmp(candidate.uid, empty, sizeof(empty)) &&
                        (candidate.status == GSTD_OIDC_UNCERTAIN ||
                         candidate.status == GSTD_OIDC_REJECTED ||
                         (job->oidc_action != GSTD_OIDC_HTTP_CREATE &&
                          candidate.status == GSTD_OIDC_PENDING) ||
                         candidate.status == success);
            } else if (job->kind == GSTD_BOOT_JOB_OIDC_HEALTH) {
                static const char empty[129] = {0};
                valid = !memcmp(candidate.uid, empty, sizeof(empty)) &&
                        (candidate.status == GSTD_OIDC_HEALTH_REJECTED ||
                         candidate.status == GSTD_OIDC_HEALTH_PENDING ||
                         candidate.status == GSTD_OIDC_HEALTH_READY);
            }
            if (valid)
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
