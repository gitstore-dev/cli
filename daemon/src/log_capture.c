#define _GNU_SOURCE 1
#include "log_capture.h"
#include "bootstrap_material.h"
#include "gitstored/credentials.h"
#include "logs.h"
#include "native_exit.h"
#include "redaction.h"
#include "oidc_material.h"
#include "oidc_config.h"
#include "workload_roles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__linux__)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#define CAPTURE_MAGIC UINT32_C(0x47534c43)
typedef struct {
    uint32_t magic;
    int32_t status;
    pid_t pid;
    uint64_t start;
    char boot[37];
} reply;
static pid_t retired[32];
static int64_t now_ms(void) {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? -1 : (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int private_directory(int fd) {
    struct stat s;
    return fd >= 3 && !fstat(fd, &s) && S_ISDIR(s.st_mode) && s.st_uid == geteuid() &&
           (s.st_mode & 07777) == 0700;
}
static int one_thread(void) {
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
static int limit(int resource, rlim_t bound) {
    struct rlimit value;
    if (getrlimit(resource, &value))
        return -1;
    if (value.rlim_cur > bound)
        value.rlim_cur = bound;
    if (value.rlim_max > bound)
        value.rlim_max = bound;
    return setrlimit(resource, &value);
}
static void wipe(void *memory, size_t count) {
    volatile unsigned char *p = memory;
    while (count--)
        *p++ = 0;
}
static int write_all(int fd, const void *input, size_t length) {
    const unsigned char *bytes = input;
    while (length) {
        ssize_t n = write(fd, bytes, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        bytes += n;
        length -= (size_t)n;
    }
    return 0;
}
static int receipt(const gstd_log_capture_input *input, const reply *r) {
    json_t *value =
        json_pack("{s:i,s:s,s:s,s:s,s:I,s:i,s:I,s:s}", "schemaVersion", 1, "storeId",
                  input->store_id, "deploymentId", input->deployment_id, "operationId",
                  input->operation_id, "generation", (json_int_t)input->generation, "pid",
                  (int)r->pid, "startTime", (json_int_t)r->start, "bootId", r->boot);
    char *bytes = value ? gstd_json_dump(value) : NULL;
    json_decref(value);
    int fd = bytes ? openat(4, "log-capture.json",
                            O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600)
                   : -1;
    int valid = fd >= 3 && !write_all(fd, bytes, strlen(bytes)) && !fsync(fd) && !fsync(4);
    if (fd >= 0)
        close(fd);
    free(bytes);
    return valid ? 0 : -1;
}
static int material(const gstd_log_capture_input *input, int oidc, gstd_credential *out) {
    json_t *request =
        json_pack("{s:{s:s,s:s,s:s,s:s},s:O}", "identity", "installation", input->store_id,
                  "issuer", oidc ? "gitstore-local/reference-oidc/1" : "gitstore-local/core/1",
                  "account", input->deployment_id, "purpose",
                  oidc ? "oidc-provider-material" : "bootstrap-material", "source",
                  oidc ? input->oidc_source : input->source);
    char *encoded = request ? gstd_json_dump(request) : NULL;
    int result =
        encoded ? gstd_credential_run(5, (unsigned char *)encoded, strlen(encoded), 5000, out) : -1;
    free(encoded);
    json_decref(request);
    return result;
}
static int secrets_from_material(const gstd_credential *material, int oidc, char *values[64], size_t *count) {
    if (oidc ? gstd_oidc_material_check(material) : gstd_bootstrap_material_check(material))
        return -1;
    json_t *value = gstd_json_parse(material->bytes, material->length);
    if (!value)
        return -1;
    const char *fields[] = {"password",   "passwordHash", "jwtSecret",
                            "hmacSecret", "apiIssuerPEM", "controllerPEM"};
    const char *oidc_fields[] = {"hydraSystem", "hydraCookie", "kratosCookie", "kratosDefault",
                                 "kratosCipher", "uiCookie", "uiCsrf", "clientSecret"};
    int valid = 1;
    for (unsigned i = 0; valid && i < (oidc ? 8u : 6u); i++) {
        const char *text = json_string_value(json_object_get(value, oidc ? oidc_fields[i] : fields[i]));
        if (!text) {
            valid = 0;
            break;
        }
        while (*text) {
            const char *end = strchr(text, '\n');
            size_t length = end ? (size_t)(end - text) : strlen(text);
            if (length && strncmp(text, "-----", 5)) {
                if (*count >= 64 || length > 4096 || !(values[*count] = strndup(text, length))) {
                    valid = 0;
                    break;
                }
                (*count)++;
            }
            if (!end)
                break;
            text = end + 1;
        }
    }
    const char *key;
    json_t *field;
    json_object_foreach(value, key, field) {
        (void)key;
        if (json_is_string(field))
            wipe((char *)json_string_value(field), json_string_length(field));
    }
    json_decref(value);
    return valid ? 0 : -1;
}
typedef struct {
    gstd_logs *logs;
    const char *deployment, *component;
    uint64_t dropped;
} sink;
static int ingest(void *context, const char *text, size_t size) {
    sink *s = context;
    int64_t now = (int64_t)time(NULL);
    if (s->dropped) {
        json_t *gap =
            json_pack("{s:s,s:I}", "code", "LOG_CAPTURE_GAP", "dropped", (json_int_t)s->dropped);
        if (gap && gstd_logs_append(s->logs, s->deployment, s->component, GSTD_STREAM_EVENTS_KIND,
                                    gap, now) == GSTD_LOG_OK)
            s->dropped = 0;
        json_decref(gap);
    }
    json_t *payload = json_object(), *value = json_stringn(text, size);
    int valid = payload && value && !json_object_set(payload, "text", value) &&
                gstd_logs_append(s->logs, s->deployment, s->component, GSTD_STREAM_LOGS, payload,
                                 now) == GSTD_LOG_OK;
    json_decref(value);
    json_decref(payload);
    if (!valid && s->dropped < GSTD_SAFE_INTEGER)
        s->dropped++;
    /* Storage failure drops redacted output with a later gap, rather than
     * stopping pipe draining or exposing raw output through another sink. */
    return 0;
}
static void run(gstd_log_capture_input input, int channel, int pipes[GSTD_CAPTURE_MAX][2][2], pid_t parent) {
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent || prctl(PR_SET_DUMPABLE, 0) ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
        _exit(126);
    int original[4 + GSTD_CAPTURE_MAX * 2] = {input.state_fd, input.configuration_fd, input.broker_fd, channel};
    unsigned descriptors = 4;
    for (unsigned i = 0; i < input.components; i++)
        for (unsigned j = 0; j < 2; j++)
            original[descriptors++] = pipes[i][j][0];
    int copied[4 + GSTD_CAPTURE_MAX * 2];
    for (unsigned i = 0; i < descriptors; i++) {
        copied[i] = original[i] < 0 ? -1 : fcntl(original[i], F_DUPFD_CLOEXEC, 32);
        if (original[i] >= 0 && copied[i] < 32)
            _exit(126);
    }
    for (unsigned i = 0; i < descriptors; i++)
        if (copied[i] < 0)
            close(3 + (int)i);
        else if (dup2(copied[i], 3 + (int)i) != 3 + (int)i ||
                 fcntl(3 + (int)i, F_SETFD, FD_CLOEXEC))
            _exit(126);
    if (syscall(SYS_close_range, 3u + descriptors, ~0u, 0u))
        _exit(126);
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0)
        _exit(126);
    for (int i = 0; i < 3; i++)
        if (dup2(null, i) < 0 || fcntl(i, F_SETFD, 0))
            _exit(126);
    if (null >= 3)
        close(null);
    if (signal(SIGXFSZ, SIG_IGN) == SIG_ERR || clearenv() || setenv("PATH", "/nonexistent", 1) ||
        setenv("LANG", "C", 1) || chdir("/") || limit(RLIMIT_CORE, 0) ||
        limit(RLIMIT_FSIZE, UINT64_C(512) * 1024 * 1024) || limit(RLIMIT_NOFILE, 64)
#ifndef GITSTORED_INSTRUMENTED
        || limit(RLIMIT_AS, UINT64_C(512) * 1024 * 1024)
#endif
    )
        _exit(126);
    reply ready = {.magic = CAPTURE_MAGIC, .status = -1, .pid = getpid()};
    char *values[64] = {0};
    size_t count = 0;
    gstd_credential secret = {0};
    gstd_logs *logs = NULL;
    gstd_redactor *redactors[GSTD_CAPTURE_MAX * 2] = {0};
    sink sinks[GSTD_CAPTURE_MAX * 2] = {0};
    const char *components[GSTD_CAPTURE_MAX] = {"git", "api", "controller"};
    unsigned core = input.components - (input.oidc ? 5u : 0u) - input.scylladb;
    if (input.scylladb)
        components[core] = "scylladb";
    if (input.oidc)
        for (unsigned i = 0; i < 5; i++)
            components[core + input.scylladb + i] = gstd_workload_name((gstd_workload_role)i);
    int log_root = gstd_logs_directory(3, input.store_id, 1);
    int narrowed = log_root >= 3 && dup2(log_root, 3) == 3 && !fcntl(3, F_SETFD, FD_CLOEXEC);
    if (log_root >= 3 && log_root != 3)
        close(log_root);
    ready.status = -10;
    int valid = narrowed && !gstd_native_boot_id(ready.boot) &&
                !gstd_native_process_start(getpid(), &ready.start);
#ifdef GITSTORED_TESTING
    if (input.test_fixture) {
        for (size_t i = 0; valid && i < input.test_secret_count; i++) {
            values[count] = strdup(input.test_secrets[i]);
            if (!values[count])
                valid = 0;
            else
                count++;
        }
    } else
#endif
    {
        if (valid && core) {
            valid = !material(&input, 0, &secret) && !secrets_from_material(&secret, 0, values, &count);
            gstd_credential_clear(&secret);
        }
        if (valid && input.oidc)
            valid = !material(&input, 1, &secret) && !secrets_from_material(&secret, 1, values, &count);
    }
    if (valid)
        ready.status = -11;
    gstd_credential_clear(&secret);
    if (fcntl(5, F_GETFD) >= 0)
        close(5);
    valid = valid && gstd_logs_open_at(3, NULL, 1, &logs) == GSTD_LOG_OK;
    if (valid)
        ready.status = -12;
    for (unsigned i = 0; valid && i < input.components * 2; i++) {
        sinks[i] =
            (sink){.logs = logs, .deployment = input.deployment_id, .component = components[i / 2]};
        redactors[i] = gstd_redactor_open((const char *const *)values, count, ingest, &sinks[i]);
        int flags = fcntl(7 + (int)i, F_GETFL);
        valid = redactors[i] && flags >= 0 && !fcntl(7 + (int)i, F_SETFL, flags | O_NONBLOCK);
    }
    for (size_t i = 0; i < count; i++) {
        wipe(values[i], strlen(values[i]));
        free(values[i]);
    }
    if (valid)
        ready.status = -13;
    valid = valid && !receipt(&input, &ready) && !prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) &&
            !prctl(PR_SET_PDEATHSIG, 0) && getppid() == parent;
    if (valid)
        ready.status = 0;
    (void)send(6, &ready, sizeof(ready), MSG_NOSIGNAL);
    close(6);
    close(4);
    if (valid) {
        struct pollfd pollers[GSTD_CAPTURE_MAX * 2];
        unsigned live = input.components * 2;
        for (unsigned i = 0; i < live; i++)
            pollers[i] = (struct pollfd){7 + (int)i, POLLIN, 0};
        int64_t maintenance = now_ms() + 60000;
        while (live) {
            int n = poll(pollers, input.components * 2, 1000);
            if (now_ms() >= maintenance) {
                (void)gstd_logs_prune(logs, (int64_t)time(NULL));
                maintenance = now_ms() + 60000;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                break;
            for (unsigned i = 0; i < input.components * 2; i++)
                if (pollers[i].fd >= 0 && pollers[i].revents) {
                    unsigned char bytes[4096];
                    ssize_t got = read(pollers[i].fd, bytes, sizeof(bytes));
                    if (got > 0) {
                        (void)gstd_redactor_feed(redactors[i], bytes, (size_t)got);
                        wipe(bytes, sizeof(bytes));
                    } else if (!got || (errno != EINTR && errno != EAGAIN)) {
                        (void)gstd_redactor_finish(redactors[i]);
                        close(pollers[i].fd);
                        pollers[i].fd = -1;
                        live--;
                    }
                }
        }
    }
    for (unsigned i = 0; i < GSTD_CAPTURE_MAX * 2; i++)
        gstd_redactor_close(redactors[i]);
    gstd_logs_close(logs);
    _exit(valid ? 0 : 126);
}
#endif
void gstd_log_capture_output_close(gstd_log_capture *capture, unsigned component) {
    if (!capture || component >= GSTD_CAPTURE_MAX)
        return;
    for (unsigned i = 0; i < 2; i++) {
        if (capture->output[component][i] >= 3)
            close(capture->output[component][i]);
        capture->output[component][i] = -1;
    }
}
void gstd_log_capture_outputs_close(gstd_log_capture *capture) {
    for (unsigned i = 0; i < GSTD_CAPTURE_MAX; i++)
        gstd_log_capture_output_close(capture, i);
}
int gstd_log_capture_launch(const gstd_log_capture_input *input, gstd_log_capture *capture) {
#if defined(__linux__)
    if (!input || !capture || capture->stage || capture->worker || !input->store_id ||
        strlen(input->store_id) != 32 || !input->deployment_id ||
        !gstd_identifier(input->deployment_id, strnlen(input->deployment_id, 65)) ||
        !input->operation_id ||
        !gstd_identifier(input->operation_id, strnlen(input->operation_id, 65)) ||
        !input->generation || input->generation > GSTD_SAFE_INTEGER || !input->components ||
        input->components > GSTD_CAPTURE_MAX || input->scylladb > 1 || input->oidc > 1 ||
        (!input->oidc && (input->components > 4 ||
            (input->scylladb && input->components < 3) ||
            (!input->scylladb && input->components == 4))) ||
        (input->oidc && (input->components < 5u + input->scylladb ||
            (input->components != 5 && input->components != 7u + input->scylladb &&
             input->components != 8u + input->scylladb))) || !private_directory(input->state_fd) ||
        !private_directory(input->configuration_fd) || !one_thread())
        return -1;
    for (unsigned i = 0; i < 32; i++)
        if (!((input->store_id[i] >= '0' && input->store_id[i] <= '9') ||
              (input->store_id[i] >= 'a' && input->store_id[i] <= 'f')))
            return -1;
#ifndef GITSTORED_TESTING
    if (input->test_fixture || input->test_secrets || input->test_secret_count)
        return -1;
#endif
    if (input->test_fixture) {
        if (input->test_secret_count > 64 || (input->test_secret_count && !input->test_secrets))
            return -1;
        for (size_t i = 0; i < input->test_secret_count; i++)
            if (!input->test_secrets[i] || !*input->test_secrets[i] ||
                strnlen(input->test_secrets[i], 4097) > 4096)
                return -1;
    } else if (input->components < 2 || input->broker_fd < 3 ||
               (input->components > (input->oidc ? 5u : 0u) && !json_is_object(input->source)) ||
               (input->oidc && !json_is_object(input->oidc_source)) ||
               input->test_secrets || input->test_secret_count)
        return -1;
    int64_t now = now_ms();
    if (now < 0)
        return -1;
    int pipes[GSTD_CAPTURE_MAX][2][2];
    for (unsigned i = 0; i < GSTD_CAPTURE_MAX; i++)
        for (unsigned j = 0; j < 2; j++)
            pipes[i][j][0] = pipes[i][j][1] = -1;
    int channels[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, channels))
        return -1;
    for (unsigned i = 0; i < input->components; i++)
        for (unsigned j = 0; j < 2; j++)
            if (pipe2(pipes[i][j], O_CLOEXEC))
                goto fail;
    pid_t parent = getpid(), child = fork();
    if (!child)
        run(*input, channels[1], pipes, parent);
    if (child < 0)
        goto fail;
    close(channels[1]);
    *capture = (gstd_log_capture)GSTD_LOG_CAPTURE_INITIAL;
    /* Capture this exact, unreaped child directly. The collector deliberately
     * sets dumpable=0 to protect credentials, which changes /proc ownership and
     * makes the native workload recovery watch predicate unsuitable here. */
    int death = (int)syscall(SYS_pidfd_open, child, 0);
    capture->death_fd = death >= 3 ? death : -1;
    capture->interrupted = death < 3;
    capture->worker = child;
    capture->stage = 1;
    capture->status = -1;
    capture->channel = channels[0];
    capture->deadline_ms = now + 10000;
    for (unsigned i = 0; i < input->components; i++)
        for (unsigned j = 0; j < 2; j++) {
            close(pipes[i][j][0]);
            capture->output[i][j] = pipes[i][j][1];
        }
    return 0;
fail:
    for (unsigned i = 0; i < GSTD_CAPTURE_MAX; i++)
        for (unsigned j = 0; j < 2; j++)
            for (unsigned k = 0; k < 2; k++)
                if (pipes[i][j][k] >= 3)
                    close(pipes[i][j][k]);
    for (unsigned i = 0; i < 2; i++)
        if (channels[i] >= 3)
            close(channels[i]);
    return -1;
#else
    (void)input;
    (void)capture;
    return -1;
#endif
}
int gstd_log_capture_poll(gstd_log_capture *capture, int cancel) {
    if (!capture || !capture->stage || cancel < -1)
        return -1;
    if (capture->stage != 1)
        return 1;
#if defined(__linux__)
    struct pollfd notification = {.fd = cancel, .events = POLLIN};
    int64_t now = now_ms();
    if (now < 0 || now >= capture->deadline_ms ||
        (cancel >= 3 && poll(&notification, 1, 0) > 0 && notification.revents))
        capture->interrupted = 1;
    char boot[37];
    uint64_t birth = 0;
    reply observed = {0};
    ssize_t count = recv(capture->channel, &observed, sizeof(observed), MSG_DONTWAIT | MSG_TRUNC);
    if (!capture->interrupted && count == sizeof(observed) && observed.magic == CAPTURE_MAGIC &&
        !observed.status && observed.pid == capture->worker && observed.boot[36] == 0 &&
        strlen(observed.boot) == 36 && capture->death_fd >= 3 &&
        !gstd_native_watch_poll(capture->death_fd) &&
        !gstd_native_process_start(capture->worker, &birth) && birth == observed.start &&
        !gstd_native_boot_id(boot) && !strcmp(boot, observed.boot)) {
        capture->stage = 2;
        capture->status = 0;
        capture->start = observed.start;
        memcpy(capture->boot, observed.boot, sizeof(capture->boot));
        close(capture->channel);
        capture->channel = -1;
        return 1;
    }
    if (count == sizeof(observed) && observed.magic == CAPTURE_MAGIC &&
        observed.pid == capture->worker && observed.status < 0)
        capture->status = observed.status;
    if (count >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        capture->interrupted = 1;
    if (capture->interrupted)
        (void)kill(capture->worker, SIGKILL);
    int status;
    pid_t reaped = waitpid(capture->worker, &status, WNOHANG);
    if (reaped == capture->worker) {
        if (capture->status == -1 && WIFEXITED(status))
            capture->status = -WEXITSTATUS(status);
        capture->worker = 0;
        capture->stage = 3;
        close(capture->channel);
        capture->channel = -1;
        gstd_log_capture_outputs_close(capture);
        return 1;
    }
    return reaped < 0 && errno != EINTR ? -1 : 0;
#else
    return -1;
#endif
}
void gstd_log_capture_reap(void) {
#if defined(__linux__)
    for (unsigned i = 0; i < 32; i++)
        if (retired[i]) {
            pid_t result = waitpid(retired[i], NULL, WNOHANG);
            if (result == retired[i] || (result < 0 && errno == ECHILD))
                retired[i] = 0;
        }
#endif
}
int gstd_log_capture_close(gstd_log_capture *capture) {
    if (!capture || capture->stage == 1)
        return -1;
    gstd_log_capture_outputs_close(capture);
#if defined(__linux__)
    if (capture->worker) {
        pid_t result = waitpid(capture->worker, NULL, WNOHANG);
        if (result < 0 && errno != ECHILD && errno != EINTR)
            return -1;
        if (!result || (result < 0 && errno == EINTR)) {
            gstd_log_capture_reap();
            unsigned i = 0;
            while (i < 32 && retired[i])
                i++;
            if (i == 32)
                return -1;
            retired[i] = capture->worker;
        }
    }
#endif
    if (capture->channel >= 3)
        close(capture->channel);
    if (capture->death_fd >= 3)
        close(capture->death_fd);
    *capture = (gstd_log_capture)GSTD_LOG_CAPTURE_INITIAL;
    return 0;
}
json_t *gstd_log_capture_identity(const gstd_log_capture *capture) {
    if (!capture || capture->stage != 2 || capture->status || capture->worker <= 0 ||
        !capture->start || gstd_native_watch_poll(capture->death_fd))
        return NULL;
    return json_pack("{s:s,s:i,s:I,s:s}", "kind", "redacted-pipes-v1", "pid", (int)capture->worker,
                     "startTime", (json_int_t)capture->start, "bootId", capture->boot);
}
